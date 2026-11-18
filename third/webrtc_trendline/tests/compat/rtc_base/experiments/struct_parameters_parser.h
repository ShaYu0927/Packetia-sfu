#pragma once
#include <memory>
#include <stdexcept>
#include <string>

namespace webrtc {
// The reference tests exercise upstream defaults only. Reject other profiles.
class StructParametersParser {
 public:
  template <typename... Args>
  static std::unique_ptr<StructParametersParser> Create(Args&&...) {
    return std::make_unique<StructParametersParser>();
  }
  void Parse(const std::string& text) {
    if (!text.empty()) throw std::logic_error("Reference supports empty field trials only");
  }
  std::string Encode() const { return {}; }
};
}
