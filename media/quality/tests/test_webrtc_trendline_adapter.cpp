#include "WebRtcTrendlineEstimator.h"
#include "webrtc_trendline/trendline_estimator.h"
#include <gtest/gtest.h>
#include <limits>

namespace media
{
TEST(WebRtcTrendlineAdapter, LargeUnsignedClockMatchesRebasedUpstreamPort)
{
    WebRtcTrendlineEstimator adapter;
    packetia::webrtc_port::TrendlineEstimator direct;
    const uint64_t origin = std::numeric_limits<uint64_t>::max() - 100000;
    uint64_t receive_ms = origin;
    for (int i = 0; i < 180; ++i)
    {
        SCOPED_TRACE(i);
        const uint64_t delta = i < 60 ? 25 : i < 120 ? 20 : 15;
        receive_ms += delta;
        const auto result = adapter.Update(delta, 20, receive_ms);
        direct.Update(delta, 20, 0, static_cast<int64_t>(receive_ms - origin), 0, true);
        if (i < 19) EXPECT_FALSE(result);
        else
        {
            ASSERT_TRUE(result);
            EXPECT_DOUBLE_EQ(result->smoothed_delay_ms, direct.SmoothedDelayMs());
            EXPECT_DOUBLE_EQ(result->slope, direct.Slope());
            EXPECT_EQ(static_cast<int>(result->usage), static_cast<int>(direct.State()));
        }
    }
}

TEST(WebRtcTrendlineAdapter, ResetAndBackwardsClockDiscardOldCongestion)
{
    WebRtcTrendlineEstimator adapter;
    std::optional<TrendlineObservation> result;
    for (int i = 0; i < 80; ++i) result = adapter.Update(25, 20, 10000 + i * 25);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->usage, BandwidthUsage::kOverUsing);
    EXPECT_FALSE(adapter.Update(20, 20, 10));
    for (int i = 0; i < 20; ++i)
    {
        result = adapter.Update(20, 20, 100 + i * 20);
        if (i < 19) EXPECT_FALSE(result);
    }
    ASSERT_TRUE(result);
    EXPECT_EQ(result->usage, BandwidthUsage::kNormal);
    EXPECT_DOUBLE_EQ(result->smoothed_delay_ms, 0);
    adapter.Reset();
    EXPECT_FALSE(adapter.Update(20, 20, 1000));
}

TEST(WebRtcTrendlineAdapter, UnrepresentableClockSpanRequiresFreshWindow)
{
    WebRtcTrendlineEstimator adapter;
    EXPECT_FALSE(adapter.Update(20, 20, 0));
    EXPECT_FALSE(adapter.Update(20, 20, std::numeric_limits<uint64_t>::max()));
    std::optional<TrendlineObservation> result;
    const uint64_t origin = std::numeric_limits<uint64_t>::max() - 1000;
    for (int i = 0; i < 20; ++i)
        result = adapter.Update(20, 20, origin + i * 20);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->usage, BandwidthUsage::kNormal);
}
}
