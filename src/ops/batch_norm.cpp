/* Copyright 2023 CMU, Facebook, LANL, MIT, NVIDIA, and Stanford (alphabetical)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flexflow/ops/batch_norm.h"
#include "flexflow/utils/hip_helper.h"
#include <hip/hip_runtime.h>

namespace FlexFlow {

// declare Legion names
using Legion::Context;
using Legion::coord_t;
using Legion::Domain;
using Legion::Machine;
using Legion::Memory;
using Legion::PhysicalRegion;
using Legion::Rect;
using Legion::Runtime;
using Legion::Task;

#define MIOPEN_BN_MIN_EPSILON 0.001

/*
  regions[0]: input
  regions[1]: output
  regions[2](I): scale
  regions[3](I): bias
*/
__host__ OpMeta *
    BatchNorm::init_task(Task const *task,
                         std::vector<PhysicalRegion> const &regions,
                         Context ctx,
                         Runtime *runtime) {
  assert(regions.size() == 4);
  assert(task->regions.size() == 4);
  BatchNorm const *bm = (BatchNorm *)task->args;
  FFHandler handle = *((FFHandler const *)task->local_args);
  TensorAccessorR<float, 4> acc_input(
      regions[0], task->regions[0], FID_DATA, ctx, runtime);
  TensorAccessorW<float, 4> acc_output(
      regions[1], task->regions[1], FID_DATA, ctx, runtime);
  TensorAccessorR<float, 1> acc_scale(
      regions[2], task->regions[2], FID_DATA, ctx, runtime);
  TensorAccessorR<float, 1> acc_bias(
      regions[3], task->regions[3], FID_DATA, ctx, runtime);

  int output_w = acc_output.rect.hi[0] - acc_output.rect.lo[0] + 1;
  int output_h = acc_output.rect.hi[1] - acc_output.rect.lo[1] + 1;
  int output_c = acc_output.rect.hi[2] - acc_output.rect.lo[2] + 1;
  int output_n = acc_output.rect.hi[3] - acc_output.rect.lo[3] + 1;

  Memory gpu_mem = get_proc_mem(Machine::get_machine(), task->target_proc);
  BatchNormMeta *m = new BatchNormMeta(
      handle, bm, gpu_mem, output_n, output_c, output_h, output_w);
  return m;
}

/*static*/
void BatchNorm::forward_kernel(BatchNormMeta *m,
                               float const *input_ptr,
                               float *output_ptr,
                               float const *scale_ptr,
                               float const *bias_ptr)
// hipStream_t stream)
{
  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));
  checkCUDNN(miopenSetStream(m->handle.dnn, stream));

  float alpha = 1.0f, beta = 0.0f;
  // coord_t numChannels = m->numChannels;
  checkCUDNN(miopenBatchNormalizationForwardTraining(
      m->handle.dnn,
      m->mode,
      &alpha,
      &beta,
      m->inputTensor,
      input_ptr,
      m->outputTensor,
      output_ptr,
      m->biasTensor,
      static_cast<void *>(const_cast<float *>(scale_ptr)),
      static_cast<void *>(const_cast<float *>(bias_ptr)),
      1.0,
      m->runningMean,
      m->runningVar,
      MIOPEN_BN_MIN_EPSILON,
      m->saveMean,
      m->saveVar));
}

/*
  regions[0](I): input
  regions[1](O): ouptut
  regions[2](I): scale
  regions[3](I): bias
*/
__host__ void
    BatchNorm::forward_task(Task const *task,
                            std::vector<PhysicalRegion> const &regions,
                            Context ctx,
                            Runtime *runtime) {
  assert(regions.size() == 4);
  assert(task->regions.size() == 4);
  // const BatchNorm* bm = (BatchNorm*) task->args;
  BatchNormMeta *m = *((BatchNormMeta **)task->local_args);
  TensorAccessorR<float, 4> acc_input(
      regions[0], task->regions[0], FID_DATA, ctx, runtime);
  TensorAccessorW<float, 4> acc_output(
      regions[1], task->regions[1], FID_DATA, ctx, runtime);
  TensorAccessorR<float, 1> acc_scale(
      regions[2], task->regions[2], FID_DATA, ctx, runtime);
  TensorAccessorR<float, 1> acc_bias(
      regions[3], task->regions[3], FID_DATA, ctx, runtime);

  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));

  hipEvent_t t_start, t_end;
  if (m->profiling) {
    checkCUDA(hipEventCreate(&t_start));
    checkCUDA(hipEventCreate(&t_end));
    checkCUDA(hipEventRecord(t_start, stream));
  }
  forward_kernel(m,
                 acc_input.ptr,
                 acc_output.ptr,
                 acc_scale.ptr,
                 acc_bias.ptr /*, stream*/);
  if (m->profiling) {
    checkCUDA(hipEventRecord(t_end, stream));
    checkCUDA(hipEventSynchronize(t_end));
    float elapsed = 0;
    checkCUDA(hipEventElapsedTime(&elapsed, t_start, t_end));
    checkCUDA(hipEventDestroy(t_start));
    checkCUDA(hipEventDestroy(t_end));
    printf("BatchNorm forward time (BF) = %.2fms\n", elapsed);
  }
}

/*static*/
void BatchNorm::backward_kernel(BatchNormMeta *m,
                                float const *input_ptr,
                                float *output_grad_ptr,
                                float const *output_ptr,
                                float *input_grad_ptr,
                                float const *scale_ptr,
                                float *scale_grad_ptr,
                                float *bias_grad_ptr,
                                size_t numElements)
// hipStream_t stream)
{
  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));
  checkCUDNN(miopenSetStream(m->handle.dnn, stream));

  float alpha = 1.0f;
  if (m->relu) {
    hipLaunchKernelGGL(reluBackward,
                       GET_BLOCKS(numElements),
                       CUDA_NUM_THREADS,
                       0,
                       stream,
                       output_grad_ptr,
                       output_ptr,
                       numElements);
  }
  checkCUDNN(miopenBatchNormalizationBackward(m->handle.dnn,
                                              m->mode,
                                              &alpha,
                                              &alpha,
                                              &alpha,
                                              &alpha,
                                              m->inputTensor,
                                              input_ptr,
                                              m->outputTensor,
                                              output_grad_ptr,
                                              m->inputTensor,
                                              input_grad_ptr,
                                              m->biasTensor,
                                              scale_ptr,
                                              scale_grad_ptr,
                                              bias_grad_ptr,
                                              MIOPEN_BN_MIN_EPSILON,
                                              m->saveMean,
                                              m->saveVar));
}

/*
  regions[0](I): input
  regions[1](I/O): input_grad
  regions[2](I): output
  regions[3](I/O): output_grad
  regions[4](I): scale
  regions[5](I/O): scale_grad
  regions[6](I/O): bias_grad
*/
__host__ void
    BatchNorm::backward_task(Task const *task,
                             std::vector<PhysicalRegion> const &regions,
                             Context ctx,
                             Runtime *runtime) {
  assert(regions.size() == 7);
  assert(task->regions.size() == 7);
  // float beta = 0.0f;
  // const BatchNorm* bm = (BatchNorm*) task->args;
  BatchNormMeta *m = *((BatchNormMeta **)task->local_args);
  TensorAccessorR<float, 4> acc_input(
      regions[0], task->regions[0], FID_DATA, ctx, runtime);
  TensorAccessorW<float, 4> acc_input_grad(regions[1],
                                           task->regions[1],
                                           FID_DATA,
                                           ctx,
                                           runtime,
                                           true /*readOutput*/);
  TensorAccessorR<float, 4> acc_output(
      regions[2], task->regions[2], FID_DATA, ctx, runtime);
  TensorAccessorW<float, 4> acc_output_grad(regions[3],
                                            task->regions[3],
                                            FID_DATA,
                                            ctx,
                                            runtime,
                                            true /*readOutput*/);
  TensorAccessorR<float, 1> acc_scale(
      regions[4], task->regions[4], FID_DATA, ctx, runtime);
  TensorAccessorW<float, 1> acc_scale_grad(regions[5],
                                           task->regions[5],
                                           FID_DATA,
                                           ctx,
                                           runtime,
                                           true /*readOutput*/);
  TensorAccessorW<float, 1> acc_bias_grad(regions[6],
                                          task->regions[6],
                                          FID_DATA,
                                          ctx,
                                          runtime,
                                          true /*readOutput*/);

  hipStream_t stream;
  checkCUDA(get_legion_stream(&stream));

  hipEvent_t t_start, t_end;
  if (m->profiling) {
    checkCUDA(hipEventCreate(&t_start));
    checkCUDA(hipEventCreate(&t_end));
    checkCUDA(hipEventRecord(t_start, stream));
  }
  backward_kernel(m,
                  acc_input.ptr,
                  acc_output_grad.ptr,
                  acc_output.ptr,
                  acc_input_grad.ptr,
                  acc_scale.ptr,
                  acc_scale_grad.ptr,
                  acc_bias_grad.ptr,
                  acc_output.rect.volume() /*, stream*/);
  if (m->profiling) {
    checkCUDA(hipEventRecord(t_end, stream));
    checkCUDA(hipEventSynchronize(t_end));
    float elapsed = 0;
    checkCUDA(hipEventElapsedTime(&elapsed, t_start, t_end));
    checkCUDA(hipEventDestroy(t_start));
    checkCUDA(hipEventDestroy(t_end));
    printf("BatchNorm backward time = %.2fms\n", elapsed);
  }
}

BatchNormMeta::BatchNormMeta(FFHandler handler,
                             BatchNorm const *bn,
                             Memory gpu_mem,
                             int output_n,
                             int output_c,
                             int output_h,
                             int output_w)
    : OpMeta(handler, bn) {
  checkCUDNN(miopenCreateTensorDescriptor(&inputTensor));
  checkCUDNN(miopenCreateTensorDescriptor(&biasTensor));
  checkCUDNN(miopenCreateTensorDescriptor(&outputTensor));
  relu = bn->relu;
  profiling = bn->profiling;
  inference_debugging = bn->inference_debugging;
  mode = miopenBNSpatial;
  // #if HIPDNN_VERSION >= 7000
  //   mode = HIPDNN_BATCHNORM_SPATIAL_PERSISTENT;
  // #endif
  fprintf(
      stderr, "output(%d,%d,%d,%d)\n", output_n, output_c, output_h, output_w);
  checkCUDNN(miopenSet4dTensorDescriptor(
      inputTensor, miopenFloat, output_n, output_c, output_h, output_w));
  checkCUDNN(miopenSet4dTensorDescriptor(
      outputTensor, miopenFloat, output_n, output_c, output_h, output_w));
  checkCUDNN(
      miopenSet4dTensorDescriptor(biasTensor, miopenFloat, 1, output_c, 1, 1));
  // allocate memory for runningMean, runningVar, saveMean, saveVar
  {
    size_t totalSize = sizeof(float) * output_c * 4;
    Realm::Rect<1, coord_t> bounds(Realm::Point<1, coord_t>(0),
                                   Realm::Point<1, coord_t>(totalSize - 1));
    std::vector<size_t> field_sizes;
    field_sizes.push_back(sizeof(char));
    Realm::RegionInstance::create_instance(reserveInst,
                                           gpu_mem,
                                           bounds,
                                           field_sizes,
                                           0,
                                           Realm::ProfilingRequestSet())
        .wait();
    runningMean = (float *)reserveInst.pointer_untyped(0, sizeof(char));
    runningVar = (float *)runningMean + output_c;
    saveMean = (float *)runningVar + output_c;
    saveVar = (float *)saveMean + output_c;
    hipStream_t stream;
    checkCUDA(get_legion_stream(&stream));
    hipLaunchKernelGGL(assign_kernel,
                       GET_BLOCKS(output_c),
                       CUDA_NUM_THREADS,
                       0,
                       stream,
                       runningMean,
                       output_c,
                       0.0f);
    hipLaunchKernelGGL(assign_kernel,
                       GET_BLOCKS(output_c),
                       CUDA_NUM_THREADS,
                       0,
                       stream,
                       runningVar,
                       output_c,
                       0.0f);
  }
  if (relu) {
    checkCUDNN(miopenCreateActivationDescriptor(&actiDesc));
    checkCUDNN(miopenSetActivationDescriptor(
        actiDesc, miopenActivationRELU, 0.0, 0.0, 0.0));
  }
}

BatchNormMeta::~BatchNormMeta(void) {
  reserveInst.destroy();
  checkCUDNN(miopenDestroyTensorDescriptor(inputTensor));
  checkCUDNN(miopenDestroyTensorDescriptor(biasTensor));
  checkCUDNN(miopenDestroyTensorDescriptor(outputTensor));
  if (relu) {
    checkCUDNN(miopenDestroyActivationDescriptor(actiDesc));
  }
}

}; // namespace FlexFlow
