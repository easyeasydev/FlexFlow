// THIS FILE WAS AUTO-GENERATED BY proj. DO NOT MODIFY IT!
// If you would like to modify this datatype, instead modify
// lib/pcg/include/pcg/optimizers/sgd_optimizer_attrs.struct.toml
/* proj-data
{
  "generated_from": "d18c91cdddc760f1fb3990d2c817ee87"
}
*/

#include "pcg/optimizers/sgd_optimizer_attrs.dtg.h"

#include <sstream>

namespace FlexFlow {
SGDOptimizerAttrs::SGDOptimizerAttrs(double const &lr,
                                     double const &momentum,
                                     bool const &nesterov,
                                     double const &weight_decay)
    : lr(lr), momentum(momentum), nesterov(nesterov),
      weight_decay(weight_decay) {}
bool SGDOptimizerAttrs::operator==(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) ==
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
bool SGDOptimizerAttrs::operator!=(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) !=
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
bool SGDOptimizerAttrs::operator<(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) <
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
bool SGDOptimizerAttrs::operator>(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) >
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
bool SGDOptimizerAttrs::operator<=(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) <=
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
bool SGDOptimizerAttrs::operator>=(SGDOptimizerAttrs const &other) const {
  return std::tie(
             this->lr, this->momentum, this->nesterov, this->weight_decay) >=
         std::tie(other.lr, other.momentum, other.nesterov, other.weight_decay);
}
} // namespace FlexFlow

namespace std {
size_t hash<FlexFlow::SGDOptimizerAttrs>::operator()(
    ::FlexFlow::SGDOptimizerAttrs const &x) const {
  size_t result = 0;
  result ^=
      std::hash<double>{}(x.lr) + 0x9e3779b9 + (result << 6) + (result >> 2);
  result ^= std::hash<double>{}(x.momentum) + 0x9e3779b9 + (result << 6) +
            (result >> 2);
  result ^= std::hash<bool>{}(x.nesterov) + 0x9e3779b9 + (result << 6) +
            (result >> 2);
  result ^= std::hash<double>{}(x.weight_decay) + 0x9e3779b9 + (result << 6) +
            (result >> 2);
  return result;
}
} // namespace std

namespace nlohmann {
::FlexFlow::SGDOptimizerAttrs
    adl_serializer<::FlexFlow::SGDOptimizerAttrs>::from_json(json const &j) {
  return ::FlexFlow::SGDOptimizerAttrs{
      j.at("lr").template get<double>(),
      j.at("momentum").template get<double>(),
      j.at("nesterov").template get<bool>(),
      j.at("weight_decay").template get<double>()};
}
void adl_serializer<::FlexFlow::SGDOptimizerAttrs>::to_json(
    json &j, ::FlexFlow::SGDOptimizerAttrs const &v) {
  j["__type"] = "SGDOptimizerAttrs";
  j["lr"] = v.lr;
  j["momentum"] = v.momentum;
  j["nesterov"] = v.nesterov;
  j["weight_decay"] = v.weight_decay;
}
} // namespace nlohmann

namespace rc {
Gen<::FlexFlow::SGDOptimizerAttrs>
    Arbitrary<::FlexFlow::SGDOptimizerAttrs>::arbitrary() {
  return gen::construct<::FlexFlow::SGDOptimizerAttrs>(
      gen::arbitrary<double>(),
      gen::arbitrary<double>(),
      gen::arbitrary<bool>(),
      gen::arbitrary<double>());
}
} // namespace rc

namespace FlexFlow {
std::string format_as(SGDOptimizerAttrs const &x) {
  std::ostringstream oss;
  oss << "<SGDOptimizerAttrs";
  oss << " lr=" << x.lr;
  oss << " momentum=" << x.momentum;
  oss << " nesterov=" << x.nesterov;
  oss << " weight_decay=" << x.weight_decay;
  oss << ">";
  return oss.str();
}
std::ostream &operator<<(std::ostream &s, SGDOptimizerAttrs const &x) {
  return s << fmt::to_string(x);
}
} // namespace FlexFlow
