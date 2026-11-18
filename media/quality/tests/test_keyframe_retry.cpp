#include "KeyFrameRetryPolicy.h"

#include <gtest/gtest.h>
#include <limits>

namespace media
{
TEST(KeyFrameRetryPolicy, MissingOrInvalidRttUsesFallback)
{
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(std::nullopt), 1000U);
    for (double rtt_ms : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity()})
        EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(rtt_ms), 1000U);
}

TEST(KeyFrameRetryPolicy, ValidRttScalesIntervalWithinBounds)
{
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(30.0), 200U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(100.0), 200U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(150.0), 300U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(400.0), 800U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(500.0), 1000U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(800.0), 1000U);
}

TEST(KeyFrameRetryPolicy, FractionalMillisecondsRoundUp)
{
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(100.1), 201U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(150.5), 301U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(499.9), 1000U);
}

TEST(KeyFrameRetryPolicy, ExtremeFiniteRttRemainsWithinBounds)
{
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(std::numeric_limits<double>::denorm_min()), 200U);
    EXPECT_EQ(ComputeKeyFrameRetryIntervalMs(std::numeric_limits<double>::max()), 1000U);
}
}
