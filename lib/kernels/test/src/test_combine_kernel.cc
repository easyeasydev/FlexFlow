#include "doctest/doctest.h"
#include "kernels/combine_kernels.h"
#include "test_utils.h"

using namespace ::FlexFlow;
TEST_SUITE(FF_TEST_SUITE) {
  TEST_CASE("Test combine kernel") {
    ManagedStream mStream = get_managed_stream();

    Allocator allocator = get_local_memory_allocator();

    TensorShape input_shape =
        make_float_tensor_shape_from_legion_dims({100, 100});

    GenericTensorAccessorW output_accessor =
        create_random_filled_accessor_w(input_shape, allocator);

    SUBCASE("forward_kernel") {
      GenericTensorAccessorR input_accessor =
          read_only_accessor_from_write_accessor(
              create_random_filled_accessor_w(input_shape, allocator));

      Kernels::Combine::forward_kernel(
          mStream.stream, input_accessor, output_accessor);

      std::vector<float> host_output_data =
          load_data_to_host_from_device<float>(
              read_only_accessor_from_write_accessor(output_accessor));
      CHECK(contains_non_zero(host_output_data));
    }

    SUBCASE("backward_kernel") {
      GenericTensorAccessorW input_accessor_grad =
          allocator.allocate_tensor(input_shape);

      Kernels::Combine::backward_kernel(
          mStream.stream,
          read_only_accessor_from_write_accessor(output_accessor),
          input_accessor_grad);

      std::vector<float> host_input_grad = load_data_to_host_from_device<float>(
          read_only_accessor_from_write_accessor(input_accessor_grad));
      CHECK(contains_non_zero(host_input_grad));
    }
  }
}
