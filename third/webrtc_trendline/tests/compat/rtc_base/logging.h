#pragma once
namespace packetia::webrtc_reference_test {
struct DiscardLog {
  template <typename T> DiscardLog& operator<<(const T&) { return *this; }
};
}
#define RTC_LOG(severity) ::packetia::webrtc_reference_test::DiscardLog{}
