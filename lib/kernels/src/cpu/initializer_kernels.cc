#include "kernels/initializer_kernels.h"
#include "kernels/datatype_dispatch.h"
#include "kernels/accessor.h"

namespace FlexFlow {

template <DataType DT>
struct ZeroInitKernel {
  void operator()(GenericTensorAccessorW const &tensor) const {
    auto arr = tensor.get<DT>();
    for (size_t i = 0; i < tensor.shape.get_volume(); i++) {
      arr[i] = 0.0f;
    }
  }
};

void zero_init_kernel(GenericTensorAccessorW const &tensor) {
  DataTypeDispatch1<ZeroInitKernel>{}(tensor.data_type, tensor);
}

template <DataType DT>
struct ConstantInitKernel {
  void operator()(GenericTensorAccessorW const &tensor, DataTypeValue value) const {
    auto arr = tensor.get<DT>();
    auto unwrapped_value = get<real_type<DT>>(value);
    for (size_t i = 0; i < tensor.shape.get_volume(); i++) {
      arr[i] = unwrapped_value;
    }
  }
};

void constant_init_kernel(GenericTensorAccessorW const &tensor, DataTypeValue value) {
  DataTypeDispatch1<ConstantInitKernel>{}(tensor.data_type, tensor, value);
}



}
