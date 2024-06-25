// THIS FILE WAS AUTO-GENERATED BY proj. DO NOT MODIFY IT!
// If you would like to modify this datatype, instead modify
// lib/op-attrs/include/op-attrs/ops/conv_2d_attrs.struct.toml
/* proj-data
{
  "generated_from": "74f98e1aacb57d847bb450e1d28d3e67"
}
*/

#ifndef _FLEXFLOW_LIB_OP_ATTRS_INCLUDE_OP_ATTRS_OPS_CONV_2D_ATTRS_DTG_H
#define _FLEXFLOW_LIB_OP_ATTRS_INCLUDE_OP_ATTRS_OPS_CONV_2D_ATTRS_DTG_H

#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "op-attrs/activation.dtg.h"
#include "rapidcheck.h"
#include "utils/json.h"
#include <functional>
#include <optional>
#include <ostream>
#include <tuple>

namespace FlexFlow {
struct Conv2DAttrs {
  Conv2DAttrs() = delete;
  explicit Conv2DAttrs(int const &out_channels,
                       int const &kernel_h,
                       int const &kernel_w,
                       int const &stride_h,
                       int const &stride_w,
                       int const &padding_h,
                       int const &padding_w,
                       int const &groups,
                       std::optional<::FlexFlow::Activation> const &activation,
                       bool const &use_bias);

  bool operator==(Conv2DAttrs const &) const;
  bool operator!=(Conv2DAttrs const &) const;
  bool operator<(Conv2DAttrs const &) const;
  bool operator>(Conv2DAttrs const &) const;
  bool operator<=(Conv2DAttrs const &) const;
  bool operator>=(Conv2DAttrs const &) const;
  int out_channels;
  int kernel_h;
  int kernel_w;
  int stride_h;
  int stride_w;
  int padding_h;
  int padding_w;
  int groups;
  std::optional<::FlexFlow::Activation> activation;
  bool use_bias;
};
} // namespace FlexFlow

namespace std {
template <>
struct hash<::FlexFlow::Conv2DAttrs> {
  size_t operator()(::FlexFlow::Conv2DAttrs const &) const;
};
} // namespace std

namespace nlohmann {
template <>
struct adl_serializer<::FlexFlow::Conv2DAttrs> {
  static ::FlexFlow::Conv2DAttrs from_json(json const &);
  static void to_json(json &, ::FlexFlow::Conv2DAttrs const &);
};
} // namespace nlohmann

namespace rc {
template <>
struct Arbitrary<::FlexFlow::Conv2DAttrs> {
  static Gen<::FlexFlow::Conv2DAttrs> arbitrary();
};
} // namespace rc

namespace FlexFlow {
std::string format_as(Conv2DAttrs const &);
std::ostream &operator<<(std::ostream &, Conv2DAttrs const &);
} // namespace FlexFlow

#endif // _FLEXFLOW_LIB_OP_ATTRS_INCLUDE_OP_ATTRS_OPS_CONV_2D_ATTRS_DTG_H
