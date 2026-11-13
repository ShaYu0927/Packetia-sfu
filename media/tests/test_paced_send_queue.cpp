#include "PacedSendQueue.h"
#include <gtest/gtest.h>

#include <map>
#include <thread>
#include <utility>

namespace
{
using Queue = media::transport::PacedSendQueue;
using Priority = Queue::Priority;
using Result = Queue::Result;
struct Harness
{
    uint64_t now = 0, next_task = 0;
    uint32_t rate = 800000;
    bool accepting = true;
    std::map<std::pair<uint64_t, uint64_t>, std::function<void()>> tasks;
    struct Sent { uint64_t at; Queue::Packet packet; };
    std::vector<Sent> sent;
    Queue queue{
        [this](uint32_t delay, auto task) {
            if (!accepting) return false;
            tasks.emplace(std::make_pair(now + delay, ++next_task), std::move(task)); return true;
        },
        [this](auto packet) { sent.push_back({now, std::move(packet)}); },
        [this] { return rate; }, [this] { return now; }};

    Result Send(Priority priority = Priority::Media, size_t size = 1200, uint8_t id = 1,
                std::function<bool()> valid = {}, std::function<void()> complete = {})
    {
        std::vector<uint8_t> packet(size, id);
        return queue.Send(priority, packet.data(), packet.size(), std::move(valid), std::move(complete));
    }
    void Advance(uint64_t end)
    {
        while (!tasks.empty() && tasks.begin()->first.first <= end)
        {
            auto task = tasks.extract(tasks.begin());
            now = std::max(now, task.key().first);
            task.mapped()();
        }
        now = end;
    }
};

TEST(MediaPacer, BurstIsCoalescedAndReleasedAtConfiguredRate)
{
    Harness h;
    for (int i = 0; i < 100; ++i) ASSERT_EQ(h.Send(), Result::Accepted);
    ASSERT_EQ(h.tasks.size(), 1U); // Not one owner task per packet.
    h.Advance(0);
    ASSERT_EQ(h.sent.size(), 1U);
    h.Advance(120);
    ASSERT_EQ(h.sent.size(), 11U);
    EXPECT_EQ(h.queue.GetStats().queued_packets, 89U);
    for (size_t i = 1; i < h.sent.size(); ++i) EXPECT_GE(h.sent[i].at - h.sent[i - 1].at, 12U);
}

TEST(MediaPacer, VideoCannotConsumeReservedAudioAndControlCapacity)
{
    Harness h;
    unsigned accepted = 0;
    while (h.Send() == Result::Accepted) ++accepted;
    EXPECT_EQ(accepted, 163U);
    for (int i = 0; i < 96; ++i) ASSERT_EQ(h.Send(Priority::Audio, 100), Result::Accepted);
    EXPECT_EQ(h.Send(Priority::Audio, 100), Result::Full);
    EXPECT_EQ(h.Send(Priority::Control, 100), Result::Accepted);
    EXPECT_EQ(h.tasks.size(), 1U);
    EXPECT_EQ(h.queue.GetStats().queued_packets, accepted + 97U);
    EXPECT_EQ(h.queue.GetStats().rejected_packets, 2U);
    h.Advance(0);
    ASSERT_EQ(h.sent.size(), 2U);
    EXPECT_EQ(h.sent[0].packet.priority, Priority::Control);
    EXPECT_EQ(h.sent[1].packet.priority, Priority::Audio);
}

TEST(MediaPacer, BlockedOwnerDoesNotCatchUpWithAnUnlimitedBurst)
{
    Harness h;
    for (int i = 0; i < 100; ++i) ASSERT_EQ(h.Send(), Result::Accepted);
    h.Advance(0);
    h.now = 1000; // Owner stalled; its timer could not run.
    h.Advance(1000);
    EXPECT_EQ(h.sent.size(), 3U); // Two packets maximum on the resumed pump.
    EXPECT_EQ(h.queue.GetStats().queued_packets, 97U);
}

TEST(MediaPacer, NewRateAppliesToFutureCreditAndZeroPausesOnlyRtp)
{
    Harness h;
    for (int i = 0; i < 10; ++i) ASSERT_EQ(h.Send(), Result::Accepted);
    h.Advance(10);
    h.rate = 80000;
    h.Advance(12);
    ASSERT_EQ(h.sent.size(), 2U);
    h.Advance(131);
    EXPECT_EQ(h.sent.size(), 2U);
    h.Advance(132);
    EXPECT_EQ(h.sent.size(), 3U);
    h.rate = 0;
    ASSERT_EQ(h.Send(Priority::Control, 20), Result::Accepted);
    h.Advance(300);
    ASSERT_EQ(h.sent.size(), 4U);
    EXPECT_EQ(h.sent.back().packet.priority, Priority::Control);
    h.rate = 800000;
    h.Advance(330);
    EXPECT_GT(h.sent.size(), 4U);
}

TEST(MediaPacer, ResetReusesWakeupAndCloseCancelsQueuedPackets)
{
    Harness h;
    int completed = 0;
    ASSERT_EQ(h.Send(Priority::Media, 1200, 1, {}, [&] { ++completed; }), Result::Accepted);
    h.queue.Reset();
    ASSERT_EQ(h.Send(Priority::Media, 1200, 2), Result::Accepted);
    EXPECT_EQ(h.tasks.size(), 1U);
    h.Advance(0);
    ASSERT_EQ(h.sent.size(), 1U);
    EXPECT_EQ(h.sent.front().packet.bytes[0], 2);
    ASSERT_EQ(h.Send(Priority::Media, 1200, 3, {}, [&] { ++completed; }), Result::Accepted);
    h.queue.Close();
    h.Advance(100);
    EXPECT_EQ(h.sent.size(), 1U);
    EXPECT_EQ(completed, 0);
    EXPECT_EQ(h.queue.GetStats().queued_bytes, 0U);
    EXPECT_EQ(h.Send(), Result::Closed);
}

TEST(MediaPacer, EnqueueDoesNotReportSocketCompletionAndValidityIsRechecked)
{
    Harness h;
    bool valid = true;
    int completed = 0;
    ASSERT_EQ(h.Send(Priority::Audio, 100, 1, [&] { return valid; }, [&] { ++completed; }), Result::Accepted);
    valid = false;
    h.Advance(0);
    EXPECT_TRUE(h.sent.empty());
    EXPECT_EQ(completed, 0);
    valid = true;
    ASSERT_EQ(h.Send(Priority::Audio, 100, 2, [&] { return valid; }, [&] { ++completed; }), Result::Accepted);
    h.Advance(10);
    ASSERT_EQ(h.sent.size(), 1U);
    EXPECT_EQ(completed, 0);
    h.sent.front().packet.on_sent(); // Only the socket completion calls this.
    EXPECT_EQ(completed, 1);
}

TEST(MediaPacer, ConcurrentProducersHaveBoundedMemoryAndOneWakeup)
{
    Harness h;
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&] { for (int j = 0; j < 128; ++j) h.Send(Priority::Media, 100); });
    for (auto& thread : producers) thread.join();
    EXPECT_EQ(h.queue.GetStats().queued_packets, 384U);
    EXPECT_EQ(h.queue.GetStats().queued_bytes, 38400U);
    EXPECT_EQ(h.queue.GetStats().rejected_packets, 1024U - 384);
    EXPECT_EQ(h.tasks.size(), 1U);
}

TEST(MediaPacer, SchedulerRejectionReleasesQueuedMemoryAndStopsAdmission)
{
    Harness h;
    h.accepting = false;
    EXPECT_EQ(h.Send(), Result::Closed);
    EXPECT_EQ(h.queue.GetStats().queued_bytes, 0U);
    EXPECT_EQ(h.Send(), Result::Closed);
}
}
