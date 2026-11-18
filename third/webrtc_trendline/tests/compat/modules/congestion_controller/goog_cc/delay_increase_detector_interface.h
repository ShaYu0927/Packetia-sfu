#pragma once
#include <cstddef>
#include <cstdint>
#include "api/transport/bandwidth_usage.h"

namespace webrtc {
class DelayIncreaseDetectorInterface {
 public:
  virtual ~DelayIncreaseDetectorInterface() = default;
  virtual void Update(double recv_delta_ms, double send_delta_ms,
                      int64_t send_time_ms, int64_t arrival_time_ms,
                      size_t packet_size, bool calculated_deltas) = 0;
  virtual BandwidthUsage State() const = 0;
};
}
