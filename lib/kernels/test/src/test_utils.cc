#include "test_utils.h"

GenericTensorAccessorW create_random_filled_accessor_w(TensorShape const &shape,
                                                       Allocator &allocator,
                                                       bool cpu_fill) {
  GenericTensorAccessorW accessor = allocator.allocate_tensor(shape);
  size_t volume = accessor.shape.num_elements();
  std::vector<float> host_data(volume);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  for (auto &val : host_data) {
    val = dist(gen);
  }

  if (cpu_fill) {
    memcpy(accessor.ptr, host_data.data(), host_data.size() * sizeof(float));
  } else {
    checkCUDA(cudaMemcpy(accessor.ptr,
                         host_data.data(),
                         host_data.size() * sizeof(float),
                         cudaMemcpyHostToDevice));
  }

  return accessor;
}

GenericTensorAccessorW create_filled_accessor_w(TensorShape const &shape,
                                                Allocator &allocator,
                                                float val,
                                                bool cpu_fill) {
  GenericTensorAccessorW accessor = allocator.allocate_tensor(shape);
  size_t volume = accessor.shape.num_elements();
  std::vector<float> host_data(volume, val);

  if (cpu_fill) {
    memcpy(accessor.ptr, host_data.data(), host_data.size() * sizeof(float));
  } else {
    checkCUDA(cudaMemcpy(accessor.ptr,
                         host_data.data(),
                         host_data.size() * sizeof(float),
                         cudaMemcpyHostToDevice));
  }

  return accessor;
}

GenericTensorAccessorW create_iota_filled_accessor_w(TensorShape const &shape,
                                                     Allocator &allocator,
                                                     bool cpu_fill) {
  GenericTensorAccessorW accessor = allocator.allocate_tensor(shape);
  size_t volume = accessor.shape.num_elements();
  std::vector<float> host_data(volume);

  for (size_t i = 0; i < volume; i++) {
    host_data[i] = i;
  }

  if (cpu_fill) {
    memcpy(accessor.ptr, host_data.data(), host_data.size() * sizeof(float));
  } else {
    checkCUDA(cudaMemcpy(accessor.ptr,
                         host_data.data(),
                         host_data.size() * sizeof(float),
                         cudaMemcpyHostToDevice));
  }

  return accessor;
}

void fill_tensor_accessor_w(GenericTensorAccessorW accessor,
                            float val,
                            bool cpu_fill) {
  LegionTensorDims dims = accessor.shape.dims;
  size_t volume = accessor.shape.num_elements();
  std::vector<float> host_data(volume, val);

  if (cpu_fill) {
    memcpy(accessor.ptr, host_data.data(), host_data.size() * sizeof(float));
  } else {
    checkCUDA(cudaMemcpy(accessor.ptr,
                         host_data.data(),
                         host_data.size() * sizeof(float),
                         cudaMemcpyHostToDevice));
  }
}

GenericTensorAccessorW
    cpu_accessor_from_gpu_accessor(TensorShape shape,
                                   GenericTensorAccessorR gpu_accessor,
                                   Allocator &cpu_allocator) {
  GenericTensorAccessorW cpu_accessor = cpu_allocator.allocate_tensor(shape);
  size_t num_elements = cpu_accessor.shape.num_elements();
  checkCUDA(cudaMemcpy(cpu_accessor.ptr,
                       gpu_accessor.ptr,
                       num_elements * sizeof(float),
                       cudaMemcpyDeviceToHost));

  return cpu_accessor;
}

TensorShape make_float_tensor_shape_from_legion_dims(FFOrdered<size_t> dims) {
  return TensorShape{
      TensorDims{
          dims,
      },
      DataType::FLOAT,
  };
}

TensorShape make_double_tensor_shape_from_legion_dims(FFOrdered<size_t> dims) {
  return TensorShape{
      TensorDims{
          dims,
      },
      DataType::DOUBLE,
  };
}
