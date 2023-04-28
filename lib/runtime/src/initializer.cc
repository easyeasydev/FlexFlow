/* Copyright 2023 CMU, Facebook, LANL, MIT, NVIDIA, and Stanford (alphabetical)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "initializer.h"
#include "tasks.h"
#include <cmath>
#include "accessor.h"
#include "task_spec.h"
#include "kernels/initializer_kernels.h"
#include "parallel_tensor.h"

namespace FlexFlow {

using namespace Legion;

GlorotUniform::GlorotUniform(int _seed) : seed(_seed) {}

UniformInitializer::UniformInitializer(int _seed, float _min, float _max)
  : seed(_seed), min_val(_min), max_val(_max)
{ }

NormInitializer::NormInitializer(int _seed, float _mean, float _stddev)
  : seed(_seed), mean(_mean), stddev(_stddev)
{ }

ConstantInitializer::ConstantInitializer(DataTypeValue const &_value)
  : value(_value)
{ }

enum GlorotSlots {
  TENSOR,
  TENSOR_DIMS,
  INITIALIZER
};

static InvocationType get_invocation_type(ParameterSyncType sync_type) {
  if (sync_type == ParameterSyncType::PS) {
    return InvocationType::STANDARD;
  } else if (sync_type == ParameterSyncType::NCCL) {
    return InvocationType::INDEX;
  } else {
    throw mk_runtime_error("Unhandled sync_type {}", sync_type);
  }
}

TaskInvocation apply_initializer(GlorotUniform const &initializer, 
                                 parallel_tensor_guid_t const &guid, 
                                 ParallelTensor const &p,
                                 TensorDims const &tensor_dims) {
  
  TaskSignature sig;
  sig.add_slot(TENSOR, { SlotType::TENSOR, WRITE_ONLY });
  sig.add_arg_slot<GlorotUniform>(INITIALIZER);
  sig.add_arg_slot<TensorDims>(TENSOR_DIMS);

  assert (tensor_dims.num_dims() >= 2);

  TaskBinding binding = { get_invocation_type(p.sync_type) };
  binding.bind(TENSOR, {guid});
  binding.bind_arg(INITIALIZER, initializer);
  binding.bind_arg(TENSOR_DIMS, tensor_dims);

  return { GLOROT_INIT_TASK_ID, binding };
}   

TaskInvocation apply_initializer(ZeroInitializer const &initializer, 
                                 parallel_tensor_guid_t const &guid, 
                                 ParallelTensor const &p) {
  TaskSignature sig;
  sig.add_slot(TENSOR, { SlotType::TENSOR, WRITE_ONLY });

  TaskBinding binding = { get_invocation_type(p.sync_type) };
  binding.bind(TENSOR, {guid});

  return { ZERO_INIT_TASK_ID, binding };
}

TaskInvocation apply_initializer(UniformInitializer const &initializer,
                                 parallel_tensor_guid_t const &guid,
                                 ParallelTensor const &p) {
  TaskSignature sig;
  sig.add_slot(TENSOR, { SlotType::TENSOR, WRITE_ONLY });
  sig.add_arg_slot<UniformInitializer>(INITIALIZER);

  TaskBinding binding = { get_invocation_type(p.sync_type) };
  binding.bind(TENSOR, {guid});
  binding.bind_arg<UniformInitializer>(INITIALIZER, initializer);

  return { UNIFORM_INIT_TASK_ID, binding };
}

TaskInvocation apply_initializer(NormInitializer const &initializer,
                                 parallel_tensor_guid_t const &guid,
                                 ParallelTensor const &p) {
  TaskSignature sig;
  sig.add_slot(TENSOR, { SlotType::TENSOR, WRITE_ONLY });
  sig.add_arg_slot<NormInitializer>(INITIALIZER);

  TaskBinding binding = { get_invocation_type(p.sync_type) };
  binding.bind(TENSOR, {guid});
  binding.bind_arg<NormInitializer>(INITIALIZER, initializer);

  return { NORMAL_INIT_TASK_ID, binding };
}

TaskInvocation apply_initializer(ConstantInitializer const &initializer,
                                 parallel_tensor_guid_t const &guid,
                                 ParallelTensor const &p) {
  TaskSignature sig;
  sig.add_slot(TENSOR, { SlotType::TENSOR, WRITE_ONLY });
  sig.add_arg_slot<ConstantInitializer>(INITIALIZER);

  TaskBinding binding = { get_invocation_type(p.sync_type) };
  binding.bind(TENSOR, {guid});
  binding.bind_arg<ConstantInitializer>(INITIALIZER, initializer);

  return { CONSTANT_INIT_TASK_ID, binding };
}


void glorot_init_task(Legion::Task const *task,
                      std::vector<Legion::PhysicalRegion> const &regions,
                      Legion::Context ctx,
                      Legion::Runtime *runtime) {
  TaskArgumentAccessor acc(task, regions, ctx, runtime);  
  auto tensor = acc.get_tensor<WRITE_ONLY>(TENSOR);
  auto initializer = acc.get_argument<GlorotUniform>(INITIALIZER);
  auto tensor_dims = acc.get_argument<TensorDims>(TENSOR_DIMS);

  auto dim = tensor_dims.crbegin();
  // reference: tensorflow code for computing fan_in/fan_out
  // https://github.com/tensorflow/tensorflow/blob/r2.0/tensorflow/python/ops/init_ops.py#L1415-L1439
  coord_t c_out = *(dim++);
  coord_t c_in = *(dim++);
  coord_t receptive_field_size = product(dim, tensor_dims.crend());
  coord_t fan_in = c_in * receptive_field_size;
  coord_t fan_out = c_out * receptive_field_size;
  float scale = sqrt(6.0f / (fan_in + fan_out));

  glorot_uniform_init_kernel(tensor, initializer.seed, scale);
}

  /* if (p.sync_type == ParameterSyncType::PS) { */
   /*  assert(p.dims.size() >= 2); */
   /*  TaskLauncher launcher(GLOROT_INIT_TASK_ID, */
   /*                        TaskArgument(this, sizeof(GlorotUniform))); */
   /*  // regions[0]: p->region */
   /*  launcher.add_region_requirement( */
   /*      RegionRequirement(backing.region, WRITE_ONLY, EXCLUSIVE, backing.region)); */
   /*  launcher.add_field(0, FID_DATA); */
   /*  runtime->execute_task(ctx, launcher); */
  /* } else if (p.sync_type == ParameterSyncType::NCCL) { */
   /*  // assert(p->owner_op != NULL); */
   /*  assert(backing.parallel_is != IndexSpace::NO_SPACE); */
   /*  assert(p.dims.size() >= 2); */
   /*  ArgumentMap argmap; */
   /*  IndexLauncher launcher(GLOROT_INIT_TASK_ID, */
   /*                         backing.parallel_is, */
   /*                         TaskArgument(this, sizeof(GlorotUniform)), */
   /*                         argmap, */
   /*                         Predicate::TRUE_PRED, */
   /*                         false, */
   /*                         0, */
   /*                         backing.mapping_id.value()); */
    // launcher.add_region_requirement(RegionRequirement(
    //     backing.part, 0 /*projection id*/, WRITE_ONLY, EXCLUSIVE, backing.region));
   /*  launcher.add_field(0, FID_DATA); */
   /*  runtime->execute_index_space(ctx, launcher); */
  /* } else { */
   /*  throw mk_runtime_error("Unhandled sync_type {}", p.sync_type); */
  /* } */
/* } */

void zero_init_task(Legion::Task const *task,
                    std::vector<Legion::PhysicalRegion> const &regions,
                    Legion::Context ctx, 
                    Legion::Runtime *runtime) {
  TaskArgumentAccessor acc(task, regions, ctx, runtime);
  auto tensor = acc.get_tensor<WRITE_ONLY>(TENSOR);

  zero_init_kernel(tensor);
}

// void ZeroInitializer::init(LegionConfig const &config, ParallelTensor const &p) {
//   Context ctx = config.lg_ctx;
//   Runtime *runtime = config.lg_hlr;
//   if (p.sync_type == ParameterSyncType::PS) {
//     ZeroInitMeta meta = { p->data_type };
//     TaskLauncher launcher(ZERO_INIT_TASK_ID,
//                           TaskArgument(&meta, sizeof(ZeroInitMeta)));
//     // regions[0]: p->region
//     launcher.add_region_requirement(
//         RegionRequirement(p->region, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_task(ctx, launcher);
//   } else if (p.sync_type == ParameterSyncType::NCCL) {
//     // assert(p->owner_op != NULL);
//     assert(p->parallel_is != IndexSpace::NO_SPACE);
//     ArgumentMap argmap;
//     ZeroInitMeta meta;
//     meta.num_regions = 1;
//     meta.data_type = p->data_type;
//     IndexLauncher launcher(ZERO_INIT_TASK_ID,
//                            p->parallel_is,
//                            TaskArgument(&meta, sizeof(ZeroInitMeta)),
//                            argmap,
//                            Predicate::TRUE_PRED,
//                            false,
//                            0,
//                            get_std_hash(p->machine_view));
//     launcher.add_region_requirement(RegionRequirement(
//         p->part, 0 /*projection id*/, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_index_space(ctx, launcher);
//   } else {
//     throw mk_runtime_error("Unhandled sync_type {}", p.sync_type);
//   }
// }


void uniform_init_task(Legion::Task const *task,
                       std::vector<Legion::PhysicalRegion> const &regions, 
                       Legion::Context ctx,
                       Legion::Runtime *runtime) {
  TaskArgumentAccessor acc(task, regions, ctx, runtime);
  auto tensor = acc.get_tensor<WRITE_ONLY>(TENSOR);
  auto initializer = acc.get_argument<UniformInitializer>(INITIALIZER);

  uniform_init_kernel(tensor, initializer.seed, initializer.min_val, initializer.max_val);
}

void norm_init_task(Legion::Task const *task, 
                    std::vector<Legion::PhysicalRegion> const &regions, 
                    Legion::Context ctx,
                    Legion::Runtime *runtime) {
  TaskArgumentAccessor acc(task, regions, ctx, runtime);
  auto tensor = acc.get_tensor<WRITE_ONLY>(TENSOR);
  auto initializer = acc.get_argument<NormInitializer>(INITIALIZER);

  norm_init_kernel(tensor, initializer.seed, initializer.mean, initializer.stddev);
}

void constant_init_task(Legion::Task const *task, 
                        std::vector<Legion::PhysicalRegion> const &regions, 
                        Legion::Context ctx,
                        Legion::Runtime *runtime) {
  TaskArgumentAccessor acc(task, regions, ctx, runtime);
  auto tensor = acc.get_tensor<WRITE_ONLY>(TENSOR);
  auto initializer = acc.get_argument<ConstantInitializer>(INITIALIZER);

  constant_init_kernel(tensor, initializer.value);
}





// void UniformInitializer::init(LegionConfig const &config, ParallelTensor const &p, ParallelTensorLegionBacking const &backing) {
//   Context ctx = config.lg_ctx;
//   Runtime *runtime = config.lg_hlr;
//   this->data_type = p.data_type;
//   if (p.sync_type == ParameterSyncType::PS) {
//     TaskLauncher launcher(UNIFORM_INIT_TASK_ID,
//                           TaskArgument(this, sizeof(UniformInitializer)));
//     // regions[0]: p->region
//     launcher.add_region_requirement(
//         RegionRequirement(backing.region, WRITE_ONLY, EXCLUSIVE, backing.region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_task(ctx, launcher);
//   } else if (psync_type == ParameterSyncType::NCCL) {
//     // assert(p->owner_op != NULL);
//     assert(p.parallel_is != IndexSpace::NO_SPACE);
//     ArgumentMap argmap;
//     IndexLauncher launcher(UNIFORM_INIT_TASK_ID,
//                            backing.parallel_is,
//                            TaskArgument(this, sizeof(UniformInitializer)),
//                            argmap,
//                            Predicate::TRUE_PRED,
//                            false,
//                            0,
//                            backing.mapping_id.value());
//     launcher.add_region_requirement(RegionRequirement(
//         backing.part, 0 /*projection id*/, WRITE_ONLY, EXCLUSIVE, backing.region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_index_space(ctx, launcher);
//   } else {
//     throw mk_runtime_error("Unhandled sync_type {}", p.sync_type);
//   }
// }
// 
// void NormInitializer::init(LegionConfig const &config, ParallelTensor const &p) {
//   Context ctx = config.lg_ctx;
//   Runtime *runtime = config.lg_hlr;
//   this->data_type = p->data_type;
//   if (p->sync_type == ParameterSyncType::PS) {
//     TaskLauncher launcher(NORMAL_INIT_TASK_ID,
//                           TaskArgument(this, sizeof(NormInitializer)));
//     // regions[0]: p->region
//     launcher.add_region_requirement(
//         RegionRequirement(p->region, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_task(ctx, launcher);
//   } else if (p->sync_type == ParameterSyncType::NCCL) {
//     // assert(p->owner_op != NULL);
//     assert(p->parallel_is != IndexSpace::NO_SPACE);
//     ArgumentMap argmap;
//     IndexLauncher launcher(NORMAL_INIT_TASK_ID,
//                            p->parallel_is,
//                            TaskArgument(this, sizeof(NormInitializer)),
//                            argmap,
//                            Predicate::TRUE_PRED,
//                            false,
//                            0,
//                            get_std_hash(p->machine_view));
//     launcher.add_region_requirement(RegionRequirement(
//         p->part, 0 /*projection id*/, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_index_space(ctx, launcher);
//   } else {
//     throw mk_runtime_error("Unhandled sync_type {}", p.sync_type);
//   }
// }
// 
// void ConstantInitializer::init(LegionConfig const &config, ParallelTensor const &p) const {
//   Context ctx = config.lg_ctx;
//   Runtime *runtime = config.lg_hlr;
//   assert(p->data_type == this->data_type);
// 
//   TaskSignature sig;
//   sig.add_arg_slot<
// 
//   if (p->sync_type == ParameterSyncType::PS) {
//     TaskLauncher launcher(CONSTANT_INIT_TASK_ID,
//                           TaskArgument(this, sizeof(ConstantInitializer)));
//     // regions[0]: p->region
//     launcher.add_region_requirement(
//         RegionRequirement(p->region, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_task(ctx, launcher);
//   } else if (p->sync_type == ParameterSyncType::NCCL) {
//     // assert(p->owner_op != NULL);
//     assert(p->parallel_is != IndexSpace::NO_SPACE);
//     ArgumentMap argmap;
//     IndexLauncher launcher(CONSTANT_INIT_TASK_ID,
//                            p->parallel_is,
//                            TaskArgument(this, sizeof(ConstantInitializer)),
//                            argmap,
//                            Predicate::TRUE_PRED,
//                            false,
//                            0,
//                            get_std_hash(p->machine_view));
//     launcher.add_region_requirement(RegionRequirement(
//         p->part, 0 /*projection id*/, WRITE_ONLY, EXCLUSIVE, p->region));
//     launcher.add_field(0, FID_DATA);
//     runtime->execute_index_space(ctx, launcher);
//   } else {
//     throw mk_runtime_error("Unhandled sync_type {}", p.sync_type);
//   }
// }

}
