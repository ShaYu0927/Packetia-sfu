#pragma once
#include <algorithm>
#include <type_traits>

namespace webrtc {
template <typename T, typename L, typename H>
auto SafeClamp(T value, L low, H high) {
  using Common = std::common_type_t<T, L, H>;
  return std::clamp(static_cast<Common>(value), static_cast<Common>(low), static_cast<Common>(high));
}
}
