#include <gtest/gtest.h>

#include "ReceiveQualityMonitor.h"

namespace
{
media::WeakNetFeedback BadReception()
{
    media::WeakNetFeedback feedback;
    feedback.now_ms = 5000;
    feedback.loss_rate = 0.25;
    feedback.receive_bitrate_bps = 500000;
    return feedback;
}

TEST(ReceiveQualityMonitor, SourcesHaveIndependentKeyframeRequestBudgets)
{
    media::ReceiveQualityMonitor monitor;
    const auto feedback = BadReception();
    ASSERT_TRUE(monitor.RegisterSource(77));
    ASSERT_TRUE(monitor.RegisterSource(88));

    const auto first = monitor.OnFeedback(77, feedback);
    EXPECT_EQ(first.target_rate.estimate.quality, media::NetworkQualityLevel::Bad);
    EXPECT_TRUE(first.request_key_frame);
    EXPECT_FALSE(monitor.OnFeedback(77, feedback).request_key_frame);
    EXPECT_TRUE(monitor.OnFeedback(88, feedback).request_key_frame);
}

TEST(ReceiveQualityMonitor, RemovingAndClearingSourcesResetOnlyTheirOwnState)
{
    media::ReceiveQualityMonitor monitor;
    const auto feedback = BadReception();
    ASSERT_TRUE(monitor.RegisterSource(77));
    ASSERT_TRUE(monitor.RegisterSource(88));
    ASSERT_TRUE(monitor.OnFeedback(77, feedback).request_key_frame);
    ASSERT_TRUE(monitor.OnFeedback(88, feedback).request_key_frame);

    monitor.Remove(77);
    monitor.Remove(77);
    EXPECT_FALSE(monitor.OnFeedback(77, feedback).HasUpdates());
    ASSERT_TRUE(monitor.RegisterSource(77));
    EXPECT_TRUE(monitor.OnFeedback(77, feedback).request_key_frame);
    EXPECT_FALSE(monitor.OnFeedback(88, feedback).request_key_frame);

    monitor.Clear();
    monitor.Clear();
    EXPECT_FALSE(monitor.OnFeedback(77, feedback).HasUpdates());
    EXPECT_FALSE(monitor.OnFeedback(88, feedback).HasUpdates());
    ASSERT_TRUE(monitor.RegisterSource(77));
    ASSERT_TRUE(monitor.RegisterSource(88));
    EXPECT_TRUE(monitor.OnFeedback(77, feedback).request_key_frame);
    EXPECT_TRUE(monitor.OnFeedback(88, feedback).request_key_frame);
}

TEST(ReceiveQualityMonitor, RegistrationCreatesUnknownStateWithoutSamplesAndPreservesExistingHistory)
{
    media::ReceiveQualityMonitor monitor;
    EXPECT_FALSE(monitor.GetSnapshot(77));
    EXPECT_FALSE(monitor.OnFeedback(77, BadReception()).HasUpdates());
    EXPECT_FALSE(monitor.GetSnapshot(77));
    ASSERT_TRUE(monitor.RegisterSource(77));
    const auto initial = monitor.GetSnapshot(77);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->quality, media::NetworkQualityLevel::Unknown);
    EXPECT_EQ(initial->update_time_ms, 0U);
    EXPECT_FALSE(initial->smoothed.loss_rate);
    EXPECT_FALSE(initial->smoothed.rtt_ms);
    EXPECT_TRUE(monitor.OnFeedback(77, BadReception()).request_key_frame);
    EXPECT_FALSE(monitor.RegisterSource(77));
    EXPECT_EQ(monitor.GetSnapshot(77)->quality, media::NetworkQualityLevel::Bad);
    EXPECT_FALSE(monitor.OnFeedback(77, BadReception()).request_key_frame);
    monitor.Remove(77);
    EXPECT_FALSE(monitor.GetSnapshot(77));
    EXPECT_FALSE(monitor.OnFeedback(77, BadReception()).HasUpdates());
    EXPECT_FALSE(monitor.GetSnapshot(77));
}
}
