#include <gtest/gtest.h>

#include "RtpSenderTrack.h"
#include "RtpTransportCcExtension.h"
#include "SendSideController.h"

#include <deque>
#include <functional>

namespace
{
class QueueTransport final : public IMediaTransport
{
public:
    uint64_t Id() const noexcept override { return 1; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::Udp; }
    MediaTransportState State() const noexcept override
    { return closed ? MediaTransportState::Closed : MediaTransportState::Connected; }
    SendResult Send(MediaPacketType, const uint8_t* data, size_t size, bool retry) override
    {
        if (closed) return SendResult::Closed;
        packets.emplace_back(data, data + size);
        retries += retry;
        return SendResult::Ok;
    }
    void Close() override { closed = true; }
    void SetPacketSink(std::weak_ptr<IMediaPacketSink>) override {}
    std::shared_ptr<media::SendSideController> GetSendSideController() override { return controller; }
    std::shared_ptr<media::SendSideController> controller;
    bool closed = false;
    size_t retries = 0;
    std::vector<std::vector<uint8_t>> packets;
};

uint16_t Sequence(const std::vector<uint8_t>& packet)
{
    return (uint16_t(packet[2]) << 8) | packet[3];
}

class NackRetransmissionQueue : public ::testing::Test
{
protected:
    void Create(rtsp::RtpSenderTrackConfig config = {})
    {
        config.local_ssrc = 33;
        sender = std::make_shared<rtsp::RtpSenderTrack>(config, transport, [this] { return now; });
        sender->SetRetransmitScheduler([this](auto task) {
            ++posts;
            if (!accept) return false;
            tasks.push_back(std::move(task));
            return true;
        });
    }
    void Send(uint16_t sequence)
    {
        const std::vector<uint8_t> packet{0x80, 96, uint8_t(sequence >> 8), uint8_t(sequence),
            0, 0, 0, 1, 0, 0, 0, 33, 0x65, 0xaa};
        ASSERT_TRUE(sender->InputRtpPacket(packet.data(), packet.size()));
    }
    void RunOne()
    {
        ASSERT_FALSE(tasks.empty());
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
    uint64_t now = 100;
    bool accept = true;
    size_t posts = 0;
    std::shared_ptr<QueueTransport> transport = std::make_shared<QueueTransport>();
    std::shared_ptr<rtsp::RtpSenderTrack> sender;
    std::deque<std::function<void()>> tasks;
};

TEST_F(NackRetransmissionQueue, CoalescesNotificationsAndYieldsBetweenBoundedBatches)
{
    rtsp::RtpSenderTrackConfig config;
    config.retransmit_batch_size = 2;
    Create(config);
    for (uint16_t seq = 100; seq <= 104; ++seq) Send(seq);
    sender->OnRtcpNack({100, 101});
    sender->OnRtcpNack({102, 103});
    sender->OnRtcpNack({104, 100});
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(posts, 1U);
    EXPECT_EQ(transport->retries, 0U);
    RunOne();
    EXPECT_EQ(transport->retries, 2U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 4U);
    ASSERT_EQ(tasks.size(), 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 4U);
    RunOne();
    EXPECT_EQ(transport->retries, 5U); // The duplicate still obeys cache retry limits.
    EXPECT_EQ(sender->GetNackQueueStats().pending, 0U);
    EXPECT_TRUE(tasks.empty());
}

TEST_F(NackRetransmissionQueue, FloodDropsOldestRequestsAndKeepsQueueBounded)
{
    rtsp::RtpSenderTrackConfig config;
    config.max_pending_nacks = 3;
    Create(config);
    for (uint16_t seq = 100; seq <= 107; ++seq) Send(seq);
    sender->OnRtcpNack({100, 101});
    sender->OnRtcpNack({102, 103});
    sender->OnRtcpNack({104, 105, 106, 107});
    EXPECT_EQ(sender->GetNackQueueStats().pending, 3U);
    EXPECT_EQ(sender->GetNackQueueStats().overflow_drops, 5U);
    ASSERT_EQ(tasks.size(), 1U);
    RunOne();
    ASSERT_EQ(transport->packets.size(), 11U);
    EXPECT_EQ(Sequence(transport->packets[8]), 105);
    EXPECT_EQ(Sequence(transport->packets[9]), 106);
    EXPECT_EQ(Sequence(transport->packets[10]), 107);
}

TEST_F(NackRetransmissionQueue, CacheCapacityAlsoBoundsPendingRequests)
{
    rtsp::RtpSenderTrackConfig config;
    config.rtp_cache_size = 2;
    Create(config);
    Send(100); Send(101);
    sender->OnRtcpNack({100, 101, 102});
    EXPECT_EQ(sender->GetNackQueueStats().pending, 2U);
    EXPECT_EQ(sender->GetNackQueueStats().overflow_drops, 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
    EXPECT_EQ(Sequence(transport->packets.back()), 101);
}

TEST_F(NackRetransmissionQueue, RejectedPostCanBeRetriedByTickWithoutNewFeedback)
{
    Create(); Send(100);
    accept = false;
    sender->OnRtcpNack({100});
    EXPECT_TRUE(tasks.empty());
    EXPECT_EQ(sender->GetNackQueueStats().post_failures, 1U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 1U);
    accept = true;
    sender->Tick(now);
    ASSERT_EQ(tasks.size(), 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
}

TEST_F(NackRetransmissionQueue, EvictedAcceptedTaskDoesNotLatchTheScheduledState)
{
    Create(); Send(100);
    sender->OnRtcpNack({100});
    ASSERT_EQ(tasks.size(), 1U);
    tasks.clear(); // Model a worker queue evicting an accepted task.
    sender->Tick(now);
    ASSERT_EQ(tasks.size(), 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
}

TEST_F(NackRetransmissionQueue, RejectedContinuationLeavesTheRemainingBatchRetryable)
{
    rtsp::RtpSenderTrackConfig config;
    config.retransmit_batch_size = 1;
    Create(config); Send(100); Send(101);
    sender->OnRtcpNack({100, 101});
    accept = false;
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
    EXPECT_TRUE(tasks.empty());
    EXPECT_EQ(sender->GetNackQueueStats().pending, 1U);
    EXPECT_EQ(sender->GetNackQueueStats().post_failures, 1U);
    accept = true;
    sender->Tick(now);
    ASSERT_EQ(tasks.size(), 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 2U);
    EXPECT_EQ(Sequence(transport->packets.back()), 101);
}

TEST_F(NackRetransmissionQueue, DiscontinuityInvalidatesOldTaskWithoutConsumingNewRequests)
{
    Create(); Send(100);
    sender->OnRtcpNack({100});
    sender->BeginDiscontinuity();
    Send(1000);
    const auto sequence = Sequence(transport->packets.back());
    sender->OnRtcpNack({sequence});
    ASSERT_EQ(tasks.size(), 2U);
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
    EXPECT_EQ(Sequence(transport->packets.back()), sequence);
}

TEST_F(NackRetransmissionQueue, PacketCanExpireWhileWaitingForTheWorker)
{
    Create(); Send(100);
    sender->OnRtcpNack({100});
    now += 500;
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 0U);
}

TEST_F(NackRetransmissionQueue, PathChangeDiscardsQueuedOldPathFeedback)
{
    transport->controller = std::make_shared<media::SendSideController>();
    Create(); Send(100);
    sender->OnRtcpNack({100});
    transport->controller->OnNetworkRouteChanged();
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 0U);
}

TEST_F(NackRetransmissionQueue, OldPathTaskCannotConsumeNewPathRequests)
{
    transport->controller = std::make_shared<media::SendSideController>();
    Create(); Send(100);
    sender->OnRtcpNack({100});
    transport->controller->OnNetworkRouteChanged();
    sender->OnRtcpNack({100});
    ASSERT_EQ(tasks.size(), 2U);
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 1U);
    RunOne();
    EXPECT_EQ(transport->retries, 1U);
}

TEST_F(NackRetransmissionQueue, ClosedTransportDropsPendingRequests)
{
    Create(); Send(100);
    sender->OnRtcpNack({100});
    transport->Close();
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
    EXPECT_EQ(sender->GetNackQueueStats().pending, 0U);
}

TEST_F(NackRetransmissionQueue, TasksDoNotKeepTheSenderAlive)
{
    Create(); Send(100);
    sender->OnRtcpNack({100});
    const std::weak_ptr<rtsp::RtpSenderTrack> weak = sender;
    sender.reset();
    EXPECT_TRUE(weak.expired());
    RunOne();
    EXPECT_EQ(transport->retries, 0U);
}

TEST_F(NackRetransmissionQueue, DeferredRetryKeepsRtpSequenceAndAllocatesFreshTwcc)
{
    rtsp::RtpSenderTrackConfig config;
    config.transport_cc_extension_id = 3;
    config.transport_sequence_allocator = std::make_shared<media::TransportSequenceAllocator>();
    Create(config); Send(100);
    sender->OnRtcpNack({100});
    EXPECT_EQ(config.transport_sequence_allocator->PeekNextExtendedSequence(), 1);
    RunOne();
    ASSERT_EQ(transport->packets.size(), 2U);
    uint16_t original = 0, retry = 0;
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(transport->packets[0].data(), transport->packets[0].size(), 3, &original));
    ASSERT_TRUE(rtsp::RtpTransportCcExtension::Read(transport->packets[1].data(), transport->packets[1].size(), 3, &retry));
    EXPECT_NE(original, retry);
    EXPECT_EQ(Sequence(transport->packets[1]), 100);
}
}
