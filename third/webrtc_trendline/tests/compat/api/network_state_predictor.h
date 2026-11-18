#pragma once
#include <cstdint>
#include "api/transport/bandwidth_usage.h"

namespace webrtc {
class NetworkStatePredictor {
 public:
  virtual ~NetworkStatePredictor() = default;
  virtual BandwidthUsage Update(int64_t send_time_ms, int64_t arrival_time_ms,
                               BandwidthUsage state) = 0;
};
}
