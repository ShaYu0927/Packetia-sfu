#include "webrtc_trendline/trendline_estimator.h"
#include "modules/congestion_controller/goog_cc/trendline_estimator.h"
#include <gtest/gtest.h>
#include <random>

namespace webrtc {
// Use the upstream friend declaration to inspect the untouched reference.
class GoogCcStatePrinter {
 public:
  static double SmoothedDelayMs(const TrendlineEstimator& e) { return e.smoothed_delay_; }
  static double Slope(const TrendlineEstimator& e) { return e.prev_trend_; }
  static double Threshold(const TrendlineEstimator& e) { return e.threshold_; }
};
}

namespace {
TEST(WebRtcTrendlinePort, MatchesUpstreamDefaultProfileAcrossNetworkChanges) {
  webrtc::FieldTrialsView defaults;
  webrtc::TrendlineEstimator reference(defaults, nullptr);
  packetia::webrtc_port::TrendlineEstimator port;
  std::mt19937 random(4817);
  int64_t send_ms = 123456789, arrival_ms = 987654321;
  for (int i = 0; i < 2400; ++i) {
    SCOPED_TRACE(i);
    const int phase = (i / 200) % 6;
    const int send_delta = 20;
    int receive_delta = phase == 1 ? 25 : phase == 3 ? 15 : 20;
    if (phase == 4) receive_delta += static_cast<int>(random() % 11) - 5;
    if (phase == 5 && i % 20 == 0) receive_delta += 100;
    send_ms += send_delta;
    arrival_ms += receive_delta;
    const bool calculated = i % 43 != 0;
    reference.Update(receive_delta, send_delta, send_ms, arrival_ms, 1200, calculated);
    port.Update(receive_delta, send_delta, send_ms, arrival_ms, 1200, calculated);
    EXPECT_EQ(static_cast<int>(port.State()), static_cast<int>(reference.State()));
    EXPECT_DOUBLE_EQ(port.SmoothedDelayMs(), webrtc::GoogCcStatePrinter::SmoothedDelayMs(reference));
    EXPECT_DOUBLE_EQ(port.Slope(), webrtc::GoogCcStatePrinter::Slope(reference));
    EXPECT_DOUBLE_EQ(port.Threshold(), webrtc::GoogCcStatePrinter::Threshold(reference));
  }
}
}
