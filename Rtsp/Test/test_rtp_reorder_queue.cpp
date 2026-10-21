#include <gtest/gtest.h>
#include "RtpPacketReorderQueue.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

namespace {
using Queue = RtpPacketReorderQueue<int>;
Queue::TimePoint At(int64_t ms) { return Queue::TimePoint{} + std::chrono::milliseconds(ms); }
void Capture(Queue& queue, std::vector<uint16_t>& result)
{
    queue.setOnPacketSorted([&](uint16_t seq, int value) {
        EXPECT_EQ(seq, value);
        result.push_back(seq);
    });
}
void Input(Queue& queue, uint16_t seq, int64_t ms) { queue.inputPacket(seq, seq, At(ms)); }
}

TEST(RtpReorderQueue, EmitsContinuousPacketsImmediatelyAndRepairsGaps)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 103, 10);
    Input(queue, 102, 20);
    EXPECT_EQ(result, (std::vector<uint16_t>{100}));
    Input(queue, 101, 30);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 101, 102, 103}));
    EXPECT_EQ(queue.getBufferedCount(), 0U);
    EXPECT_EQ(queue.getLostCount(), 0U);
}

TEST(RtpReorderQueue, ReordersAcrossWrapAndAcrossMultipleCycles)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 65534, 0);
    Input(queue, 1, 1);
    Input(queue, 0, 2);
    Input(queue, 65535, 3);
    EXPECT_EQ(result, (std::vector<uint16_t>{65534, 65535, 0, 1}));
    for (int i = 2; i < 131080; ++i) Input(queue, static_cast<uint16_t>(i), 4);
    EXPECT_EQ(queue.getLostCount(), 0U);
    EXPECT_EQ(queue.getDropCount(), 0U);
    EXPECT_EQ(queue.getReorderStats().emitted, 131082U);
}

TEST(RtpReorderQueue, TimeoutSkipsNearestExtendedSequenceAtWrap)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 65533, 0);
    Input(queue, 0, 10);
    Input(queue, 65535, 20);
    queue.flushExpired(At(109));
    EXPECT_EQ(result.size(), 1U);
    queue.flushExpired(At(110));
    EXPECT_EQ(result, (std::vector<uint16_t>{65533, 65535, 0}));
    EXPECT_EQ(queue.getLostCount(), 1U);
    EXPECT_EQ(queue.getBufferedCount(), 0U);
}

TEST(RtpReorderQueue, DeadlineStartsAtBlockedArrivalRatherThanPreviousFlush)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 10, 0);
    Input(queue, 12, 1000);
    queue.flushExpired(At(1099));
    EXPECT_EQ(result, (std::vector<uint16_t>{10}));
    queue.flushExpired(At(1100));
    EXPECT_EQ(result, (std::vector<uint16_t>{10, 12}));
    EXPECT_EQ(queue.getLostCount(), 1U);
}

TEST(RtpReorderQueue, NewTrafficAndDuplicatesCannotExtendOldestDeadline)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 10, 0);
    Input(queue, 14, 10);
    Input(queue, 12, 90);
    Input(queue, 14, 100);
    queue.flushExpired(At(110));
    EXPECT_EQ(result, (std::vector<uint16_t>{10, 12, 14}));
    EXPECT_EQ(queue.getLostCount(), 2U);
    EXPECT_EQ(queue.getReorderStats().duplicates, 1U);
}

TEST(RtpReorderQueue, ARepairedGapDoesNotExpireTheNextNewerGapEarly)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 10, 0);
    Input(queue, 12, 10);
    Input(queue, 14, 90);
    Input(queue, 11, 99);
    queue.flushExpired(At(110));
    EXPECT_EQ(result, (std::vector<uint16_t>{10, 11, 12}));
    queue.flushExpired(At(190));
    EXPECT_EQ(result, (std::vector<uint16_t>{10, 11, 12, 14}));
}

TEST(RtpReorderQueue, RejectsLateRepeatsAmbiguousAndOutOfWindowPackets)
{
    Queue queue(1000, 50, 100);
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 102, 1);
    Input(queue, 102, 2);
    Input(queue, 100, 3);
    Input(queue, 1101, 4); // Window is exclusive: next=101, distance=1000.
    Input(queue, static_cast<uint16_t>(101 + 0x8000), 5);
    queue.flushExpired(At(101));
    Input(queue, 101, 102);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 102}));
    EXPECT_EQ(queue.getReorderStats().duplicates, 1U);
    EXPECT_EQ(queue.getReorderStats().late, 2U);
    EXPECT_EQ(queue.getReorderStats().out_of_window, 2U);
    EXPECT_EQ(queue.getDropCount(), 5U);
    EXPECT_EQ(queue.getLostCount(), 1U);
}

TEST(RtpReorderQueue, CapacityAdvancesOnlyAsFarAsNecessary)
{
    Queue queue(1000, 2, 100);
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 104, 1);
    Input(queue, 106, 2);
    Input(queue, 102, 3);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 102}));
    EXPECT_EQ(queue.getBufferedCount(), 2U);
    EXPECT_EQ(queue.getLostCount(), 1U);
    EXPECT_EQ(queue.getReorderStats().overflows, 1U);
    Input(queue, 103, 4);
    Input(queue, 105, 5);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 102, 103, 104, 105, 106}));
}

TEST(RtpReorderQueue, SupportsNoWaitNoCacheAndExplicitDrain)
{
    for (const auto no_cache : {false, true}) {
        Queue queue(1000, no_cache ? 0 : 50, no_cache ? 100 : 0);
        std::vector<uint16_t> result;
        Capture(queue, result);
        Input(queue, 10, 0);
        Input(queue, 12, 1);
        EXPECT_EQ(result, (std::vector<uint16_t>{10, 12}));
        EXPECT_EQ(queue.getLostCount(), 1U);
        EXPECT_EQ(queue.getBufferedCount(), 0U);
    }
    Queue queue;
    Input(queue, 100, 0);
    Input(queue, 103, 1);
    Input(queue, 105, 2);
    queue.flushTimeout();
    EXPECT_EQ(queue.getLostCount(), 3U);
    EXPECT_EQ(queue.getBufferedCount(), 0U);
}

TEST(RtpReorderQueue, ResetReleasesOwnershipAndAcceptsANewSourceEpoch)
{
    RtpPacketReorderQueue<std::shared_ptr<int>> queue;
    queue.inputPacket(100, std::make_shared<int>(1), At(0));
    auto packet = std::make_shared<int>(2);
    std::weak_ptr<int> weak = packet;
    queue.inputPacket(102, std::move(packet), At(1));
    EXPECT_FALSE(weak.expired());
    queue.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(queue.getBufferedCount(), 0U);
    EXPECT_EQ(queue.getReorderStats().emitted, 0U);
    queue.inputPacket(5, std::make_shared<int>(3), At(2));
    EXPECT_EQ(queue.getReorderStats().emitted, 1U);
}

TEST(RtpReorderQueue, RandomizedReorderingRetainsEveryPacketAcrossWrap)
{
    Queue queue(1000, 100, 100);
    std::vector<uint16_t> result;
    Capture(queue, result);
    std::mt19937 random(12345);
    Input(queue, 65500, 0);
    for (int start = 65501; start < 65501 + 20000; start += 40) {
        std::vector<int> batch(40);
        std::iota(batch.begin(), batch.end(), start);
        std::shuffle(batch.begin(), batch.end(), random);
        for (const auto seq : batch) Input(queue, static_cast<uint16_t>(seq), 1);
    }
    ASSERT_EQ(result.size(), 20001U);
    for (size_t i = 0; i < result.size(); ++i)
        ASSERT_EQ(result[i], static_cast<uint16_t>(65500 + i));
    EXPECT_EQ(queue.getLostCount(), 0U);
    EXPECT_EQ(queue.getDropCount(), 0U);
}

TEST(RtpReorderQueue, ValidatesSequenceWindow)
{
    EXPECT_THROW(Queue(0), std::invalid_argument);
    EXPECT_THROW(Queue(32769), std::invalid_argument);
    EXPECT_THROW(Queue(1000, 32768), std::invalid_argument);
}

TEST(RtpReorderQueue, RecoversForwardAfterLongLossOnlyOnThirdConsecutiveProbe)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 799, 0);
    Input(queue, 2000, 1200);
    Input(queue, 2001, 1201);
    EXPECT_EQ(result, (std::vector<uint16_t>{799}));
    EXPECT_EQ(queue.getBufferedCount(), 0U); // Probes retain no payload.
    Input(queue, 2002, 1202);
    Input(queue, 2003, 1203);
    Input(queue, 800, 1204); // Old recovery/retransmission must not revive the gap.
    EXPECT_EQ(result, (std::vector<uint16_t>{799, 2002, 2003}));
    EXPECT_EQ(queue.getLostCount(), 1202U);
    EXPECT_EQ(queue.getReorderStats().out_of_window, 2U);
    EXPECT_EQ(queue.getReorderStats().late, 1U);
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 1U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
}

TEST(RtpReorderQueue, ConsecutiveOutliersCannotReplaceAnActiveStream)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    for (int i = 0; i < 3; ++i) Input(queue, 2000 + i, 100 + i);
    Input(queue, 101, 103);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 101}));
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 0U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
}

TEST(RtpReorderQueue, NormalTrafficCancelsUnconfirmedResync)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 2000, 500);
    Input(queue, 2001, 501);
    Input(queue, 101, 502);
    Input(queue, 2002, 1002);
    Input(queue, 2003, 1003);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 101}));
    Input(queue, 2004, 1004);
    EXPECT_EQ(result.back(), 2004);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
}

TEST(RtpReorderQueue, BufferedNormalTrafficCancelsAndBlocksProbation)
{
    Queue queue(1000, 50, 1000);
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 2000, 500);
    Input(queue, 102, 501); // An in-window gap clears the candidate, even without output.
    Input(queue, 2001, 502);
    Input(queue, 2002, 503);
    Input(queue, 2003, 504);
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 1U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
    EXPECT_EQ(queue.getBufferedCount(), 1U);
    Input(queue, 101, 505);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 101, 102}));
}

TEST(RtpReorderQueue, DrainingExpiredOldCacheRestartsStallGuard)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 102, 1);
    Input(queue, 2000, 600); // First releases 102, so this is no longer stalled.
    Input(queue, 2001, 601);
    Input(queue, 2002, 602);
    EXPECT_EQ(result, (std::vector<uint16_t>{100, 102}));
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 0U);
}

TEST(RtpReorderQueue, DuplicateProbesCannotConfirmOrExtendProbation)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 100, 0);
    Input(queue, 2000, 500);
    Input(queue, 2000, 550);
    Input(queue, 2001, 599);
    EXPECT_EQ(result.size(), 1U);
    Input(queue, 2002, 600); // Expired at exactly 100 ms; starts a new run.
    Input(queue, 2003, 601);
    EXPECT_EQ(result.size(), 1U);
    Input(queue, 2004, 602);
    EXPECT_EQ(result.back(), 2004);
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 2U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
}

TEST(RtpReorderQueue, NonconsecutiveForwardProbesRestartConfirmation)
{
    Queue queue;
    Input(queue, 100, 0);
    Input(queue, 2000, 500);
    Input(queue, 2002, 501);
    Input(queue, 2003, 502);
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
    Input(queue, 2004, 503);
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 2U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
}

TEST(RtpReorderQueue, ResyncProbeRunCanCrossSequenceWrap)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 64000, 0);
    Input(queue, 65535, 500);
    Input(queue, 0, 501);
    Input(queue, 1, 502);
    Input(queue, 2, 503);
    EXPECT_EQ(result, (std::vector<uint16_t>{64000, 1, 2}));
    EXPECT_EQ(queue.getLostCount(), 1536U);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
}

TEST(RtpReorderQueue, LateAndHalfCycleAmbiguousPacketsNeverConfirmResync)
{
    Queue queue;
    std::vector<uint16_t> result;
    Capture(queue, result);
    Input(queue, 5000, 0);
    Input(queue, 100, 500);
    Input(queue, 101, 501);
    Input(queue, 102, 502);
    Input(queue, 7000, 503);
    Input(queue, 7001, 504);
    Input(queue, static_cast<uint16_t>(5001 + 0x8000), 505); // Cancels the run.
    Input(queue, 7002, 506);
    Input(queue, 5001, 507);
    EXPECT_EQ(result, (std::vector<uint16_t>{5000, 5001}));
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
}

TEST(RtpReorderQueue, PeriodicExpiryAndResetCancelProbation)
{
    Queue queue;
    Input(queue, 100, 0);
    Input(queue, 2000, 500);
    Input(queue, 2001, 501);
    queue.flushExpired(At(600));
    Input(queue, 2002, 601);
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
    queue.reset();
    Input(queue, 100, 602);
    Input(queue, 2003, 1102);
    Input(queue, 2004, 1103);
    EXPECT_EQ(queue.getReorderStats().resyncs, 0U);
    Input(queue, 2005, 1104);
    EXPECT_EQ(queue.getReorderStats().resyncs, 1U);
    EXPECT_EQ(queue.getReorderStats().resync_attempts, 1U);
}

TEST(RtpReorderQueue, ProbationDoesNotRetainPacketOwnership)
{
    RtpPacketReorderQueue<std::shared_ptr<int>> queue;
    queue.inputPacket(100, std::make_shared<int>(100), At(0));
    auto packet = std::make_shared<int>(2000);
    const std::weak_ptr<int> weak = packet;
    queue.inputPacket(2000, std::move(packet), At(500));
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(queue.getBufferedCount(), 0U);
}

TEST(RtpReorderQueue, SupportsCustomOrDisabledResyncPolicy)
{
    Queue disabled(1000, 50, 100, {0, 500, 100});
    Input(disabled, 100, 0);
    for (int i = 0; i < 5; ++i) Input(disabled, 2000 + i, 500 + i);
    EXPECT_EQ(disabled.getReorderStats().resync_attempts, 0U);
    EXPECT_EQ(disabled.getReorderStats().emitted, 1U);

    Queue custom(1000, 50, 100, {4, 200, 50});
    Input(custom, 100, 0);
    Input(custom, 1999, 199); // Cannot count toward the run before the stall threshold.
    Input(custom, 2000, 200);
    Input(custom, 2001, 201);
    Input(custom, 2002, 202);
    EXPECT_EQ(custom.getReorderStats().resyncs, 0U);
    Input(custom, 2003, 203);
    EXPECT_EQ(custom.getReorderStats().resyncs, 1U);
    EXPECT_THROW((Queue(1000, 50, 100, {1, 500, 100})), std::invalid_argument);
    EXPECT_THROW((Queue(1000, 50, 100, {3, 0, 100})), std::invalid_argument);
    EXPECT_THROW((Queue(1000, 50, 100, {3, 500, 0})), std::invalid_argument);
}
