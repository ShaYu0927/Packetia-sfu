#pragma once
#include <string>
#include <string_view>

namespace webrtc {
// Test-only default profile. No experiments can be enabled through this shim.
class FieldTrialsView {
 public:
  std::string Lookup(std::string_view) const { return {}; }
  bool IsEnabled(std::string_view) const { return false; }
};
}
