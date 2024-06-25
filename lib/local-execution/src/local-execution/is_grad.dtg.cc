// THIS FILE WAS AUTO-GENERATED BY proj. DO NOT MODIFY IT!
// If you would like to modify this datatype, instead modify
// lib/local-execution/include/local-execution/is_grad.enum.toml
/* proj-data
{
  "generated_from": "1d95f91d91a4f831a426e5cb7b1ae5be"
}
*/

#include "local-execution/is_grad.dtg.h"

#include <sstream>
#include <stdexcept>

namespace std {
size_t hash<FlexFlow::IsGrad>::operator()(FlexFlow::IsGrad x) const {
  return std::hash<int>{}(static_cast<int>(x));
}
} // namespace std
namespace FlexFlow {
std::string format_as(IsGrad x) {
  switch (x) {
    case IsGrad::YES:
      return "YES";
    case IsGrad::NO:
      return "NO";
    default:
      std::ostringstream oss;
      oss << "Unknown IsGrad value " << static_cast<int>(x);
      throw std::runtime_error(oss.str());
  }
}
std::ostream &operator<<(std::ostream &s, IsGrad x) {
  return s << fmt::to_string(x);
}
} // namespace FlexFlow
namespace FlexFlow {
void to_json(::nlohmann::json &j, IsGrad x) {
  switch (x) {
    case IsGrad::YES:
      j = "YES";
      break;
    case IsGrad::NO:
      j = "NO";
      break;
    default:
      std::ostringstream oss;
      oss << "Unknown IsGrad value " << static_cast<int>(x);
      throw std::runtime_error(oss.str());
  }
}
void from_json(::nlohmann::json const &j, IsGrad &x) {
  std::string as_str = j.get<std::string>();
  if (as_str == "YES") {
    x = IsGrad::YES;
  } else if (as_str == "NO") {
    x = IsGrad::NO;
  } else {
    std::ostringstream oss;
    oss << "Unknown IsGrad value " << as_str;
    throw std::runtime_error(oss.str());
  }
}
} // namespace FlexFlow
namespace rc {
Gen<FlexFlow::IsGrad> Arbitrary<FlexFlow::IsGrad>::arbitrary() {
  return gen::element<FlexFlow::IsGrad>(FlexFlow::IsGrad::YES,
                                        FlexFlow::IsGrad::NO);
}
} // namespace rc
