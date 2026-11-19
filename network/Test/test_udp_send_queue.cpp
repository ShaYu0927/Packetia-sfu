#include "UdpSendQueue.h"
#include "EventLoop.h"
#include "RuntimeMetrics.h"

#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <future>
#include <poll.h>
#include <unistd.h>
#include <vector>
#include <stdexcept>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace network::transport;

class UdpSendQueueTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(loop.Start());
        owner = loop.GetTaskScheduler();
        server = std::make_shared<network::UdpServer>(owner);
        ASSERT_TRUE(server->Start("127.0.0.1", 0));
        receiver = ::socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(receiver, 0);
        remote = network::SocketAddr::FromIPPort("127.0.0.1", 0);
        ASSERT_EQ(::bind(receiver, reinterpret_cast<const sockaddr*>(&remote.ss), remote.len), 0);
        ASSERT_EQ(::getsockname(receiver, reinterpret_cast<sockaddr*>(&remote.ss), &remote.len), 0);
        budget = std::make_shared<utils::ChannelBudget>();
    }
    void TearDown() override
    {
        ResumeNetwork();
        if (queue) queue->Cancel();
        if (server) server->Stop();
        loop.Stop();
        if (receiver >= 0) ::close(receiver);
    }
    void CreateQueue(utils::ChannelLimits limits = UdpSendQueue::kDefaultLimits)
    {
        // Capacity/lifetime tests must not depend on wall-clock scheduling.
        UdpSendQueue::Options options;
        options.max_queue_age_ms = 0;
        CreateQueue(limits, std::move(options));
    }
    void CreateQueue(utils::ChannelLimits limits, UdpSendQueue::Options options)
    {
        queue = std::make_shared<UdpSendQueue>(owner, server, budget, limits, std::move(options));
        adapter = std::make_shared<UdpDatagramTransport>(1, server, queue->MakeSender());
    }
    bool PauseNetwork()
    {
        release = std::make_shared<std::promise<void>>();
        auto entered = std::make_shared<std::promise<void>>();
        auto ready = entered->get_future();
        const auto gate = release->get_future().share();
        return owner->Post([entered, gate] { entered->set_value(); gate.wait_for(3s); }) &&
            ready.wait_for(1s) == std::future_status::ready;
    }
    void ResumeNetwork()
    {
        if (!release) return;
        release->set_value();
        release.reset();
        owner->Invoke([] {});
    }
    DatagramSendResult Send(const std::vector<uint8_t>& payload)
    {
        return adapter->SendDatagram(remote, payload.data(), payload.size());
    }
    std::vector<uint8_t> Receive(int timeout_ms = 1000)
    {
        pollfd event{receiver, POLLIN, 0};
        if (::poll(&event, 1, timeout_ms) != 1) return {};
        std::vector<uint8_t> payload(1024);
        const auto size = ::recv(receiver, payload.data(), payload.size(), 0);
        if (size < 0) return {};
        payload.resize(static_cast<std::size_t>(size));
        return payload;
    }

    EventLoop loop{1};
    std::shared_ptr<TaskScheduler> owner;
    std::shared_ptr<network::UdpServer> server;
    std::shared_ptr<utils::ChannelBudget> budget;
    std::shared_ptr<UdpSendQueue> queue;
    std::shared_ptr<UdpDatagramTransport> adapter;
    std::shared_ptr<std::promise<void>> release;
    int receiver = -1;
    network::SocketAddr remote;
};

TEST_F(UdpSendQueueTest, OwnsQueuedBytesAndPreservesOrderWithMessageBackpressure)
{
    CreateQueue({1024 * 1024, 2});
    ASSERT_TRUE(PauseNetwork());
    std::vector<uint8_t> payload{1, 2, 3};
    EXPECT_EQ(Send(payload), DatagramSendResult::Ok);
    payload = {4, 5, 6};
    EXPECT_EQ(Send(payload), DatagramSendResult::Ok);
    payload.assign(3, 0);
    EXPECT_EQ(Send(payload), DatagramSendResult::NotWritable);
    EXPECT_EQ(budget->Used().second, 2u);
    ResumeNetwork();
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{4, 5, 6}));
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(budget->Used().first, 0u);
}

TEST_F(UdpSendQueueTest, ClosedAdapterAndCancelledQueueDoNotSendPendingDatagrams)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    EXPECT_EQ(Send({1}), DatagramSendResult::Ok);
    adapter->Close();
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(budget->Used().second, 0u);

    adapter = std::make_shared<UdpDatagramTransport>(2, server, queue->MakeSender());
    ASSERT_TRUE(PauseNetwork());
    EXPECT_EQ(Send({2}), DatagramSendResult::Ok);
    queue->Cancel(); queue->Cancel();
    EXPECT_EQ(budget->Used().second, 0u);
    EXPECT_EQ(Send({3}), DatagramSendResult::Closed);
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
}

TEST_F(UdpSendQueueTest, CompletionRunsAfterSocketSendAndStaleDatagramsAreDropped)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    const std::vector<uint8_t> payload{4, 5};
    std::atomic<bool> valid{true};
    std::atomic<int> completed{0};
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, payload.data(), payload.size(),
        [&] { return valid.load(); }, [&] { ++completed; }), DatagramSendResult::Ok);
    EXPECT_EQ(completed.load(), 0);
    ResumeNetwork();
    EXPECT_EQ(Receive(), payload);
    EXPECT_EQ(completed.load(), 1);

    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, payload.data(), payload.size(),
        [&] { return valid.load(); }, [&] { ++completed; }), DatagramSendResult::Ok);
    valid.store(false);
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(completed.load(), 1);
}

TEST_F(UdpSendQueueTest, QueuesShareByteBudgetAndDestroyingQueueReleasesPendingBytes)
{
    const std::vector<uint8_t> payload{7, 8, 9};
    const auto bytes = common::SharedBuffer::TryCopy(payload.data(), payload.size()).StorageSize();
    budget = std::make_shared<utils::ChannelBudget>(bytes, 2);
    CreateQueue();
    auto otherQueue = std::make_shared<UdpSendQueue>(owner, server, budget);
    auto otherAdapter = std::make_shared<UdpDatagramTransport>(2, server, otherQueue->MakeSender());
    ASSERT_TRUE(PauseNetwork());
    EXPECT_EQ(Send(payload), DatagramSendResult::Ok);
    EXPECT_EQ(otherAdapter->SendDatagram(remote, payload.data(), payload.size()), DatagramSendResult::NotWritable);
    queue.reset();
    EXPECT_EQ(budget->Used().first, 0u);
    EXPECT_EQ(Send(payload), DatagramSendResult::Closed);
    EXPECT_EQ(otherAdapter->SendDatagram(remote, payload.data(), payload.size()), DatagramSendResult::Ok);
    ResumeNetwork();
    EXPECT_EQ(Receive(), payload);
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(budget->Used().first, 0u);
}

TEST_F(UdpSendQueueTest, ExpiredPacketsReleaseCapacityAndDoNotCompleteOrReachSocket)
{
    std::atomic<uint64_t> now{1000};
    UdpSendQueue::Options options;
    options.clock = [&] { return now.load(); };
    CreateQueue({1024, 1}, options);
    ASSERT_TRUE(PauseNetwork());
    const std::vector<uint8_t> payload{1, 2};
    std::atomic<int> completed{0};
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, payload.data(), payload.size(), {},
        [&] { ++completed; }), DatagramSendResult::Ok);
    EXPECT_EQ(queue->GetStats().pending_packets, 1u);
    now = 1100;
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(completed.load(), 0);
    auto stats = queue->GetStats();
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Expired)], 1u);
    EXPECT_EQ(stats.pending_packets, 0u);
    EXPECT_EQ(stats.pending_bytes, 0u);
    EXPECT_EQ(budget->Used().first, 0u);
    ASSERT_EQ(Send(payload), DatagramSendResult::Ok);
    EXPECT_EQ(Receive(), payload);
    owner->Invoke([] {});
    stats = queue->GetStats();
    EXPECT_EQ(stats.enqueued, 2u);
    EXPECT_EQ(stats.sent_packets, 1u);
    EXPECT_EQ(stats.sent_bytes, payload.size());
}

TEST_F(UdpSendQueueTest, BackpressureDropsOnceAndContinuesToNextPacket)
{
    monitoring::MetricsContext context;
    std::atomic<int> attempts{0}, completed{0};
    UdpSendQueue::Options options;
    options.send = [&](const auto& address, const uint8_t* data, size_t size) {
        EXPECT_TRUE(owner->IsCurrentThread());
        if (++attempts == 1) return network::UdpServer::SendResult::NotWritable;
        return server->TrySendTo(address, data, size);
    };
    CreateQueue(UdpSendQueue::kDefaultLimits, options);
    ASSERT_TRUE(PauseNetwork());
    const std::vector<uint8_t> payload{4};
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, payload.data(), payload.size(), {},
        [&] { ++completed; }), DatagramSendResult::Ok);
    ASSERT_EQ(Send({5}), DatagramSendResult::Ok);
    ResumeNetwork();
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{5}));
    EXPECT_TRUE(Receive(20).empty());
    EXPECT_EQ(attempts.load(), 2);
    EXPECT_EQ(completed.load(), 0);
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Blocked)], 1u);
    EXPECT_EQ(stats.sent_packets, 1u);
    EXPECT_EQ(stats.pending_packets, 0u);
    const auto traffic = context.Registry()->Collect().send[static_cast<size_t>(monitoring::Transport::Udp)];
    EXPECT_EQ(traffic.abandoned, 1u);
    EXPECT_EQ(traffic.drops[static_cast<size_t>(monitoring::SendDropReason::Blocked)], 1u);
}

TEST_F(UdpSendQueueTest, ExplicitlyDisablingExpiryRetainsPacketsWithinCapacity)
{
    std::atomic<uint64_t> now{1000};
    UdpSendQueue::Options options;
    options.max_queue_age_ms = 0;
    options.clock = [&] { return now.load(); };
    CreateQueue({1024, 1}, options);
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send({9}), DatagramSendResult::Ok);
    now = 100000;
    EXPECT_EQ(Send({10}), DatagramSendResult::NotWritable);
    ResumeNetwork();
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{9}));
    owner->Invoke([] {});
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.sent_packets, 1u);
    EXPECT_EQ(stats.rejected_full, 1u);
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Expired)], 0u);
}

TEST_F(UdpSendQueueTest, DestructionClassifiesAllQueuedPacketsAndReleasesSharedBudget)
{
    monitoring::MetricsContext context;
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send({1}), DatagramSendResult::Ok);
    ASSERT_EQ(Send({2}), DatagramSendResult::Ok);
    queue.reset();
    EXPECT_EQ(budget->Used().second, 0u);
    auto traffic = context.Registry()->Collect().send[static_cast<size_t>(monitoring::Transport::Udp)];
    EXPECT_EQ(traffic.abandoned, 2u);
    EXPECT_EQ(traffic.drops[static_cast<size_t>(monitoring::SendDropReason::Cancelled)], 2u);
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
    traffic = context.Registry()->Collect().send[static_cast<size_t>(monitoring::Transport::Udp)];
    EXPECT_EQ(traffic.abandoned, 2u);
}

TEST_F(UdpSendQueueTest, CancellationAndInvalidationHaveDistinctTerminalCounts)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    const std::vector<uint8_t> payload{6};
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, payload.data(), payload.size(),
        [] { return false; }, {}), DatagramSendResult::Ok);
    ResumeNetwork();
    EXPECT_TRUE(Receive(20).empty());
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send(payload), DatagramSendResult::Ok);
    queue->Cancel();
    queue->Cancel();
    EXPECT_EQ(Send(payload), DatagramSendResult::Closed);
    ResumeNetwork();
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.enqueued, 2u);
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Invalidated)], 1u);
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Cancelled)], 1u);
    EXPECT_EQ(stats.rejected_closed, 1u);
    EXPECT_EQ(stats.pending_packets, 0u);
    EXPECT_EQ(budget->Used().second, 0u);
}

TEST_F(UdpSendQueueTest, SocketFailureAndClosedAdapterDoNotReportSuccessfulCompletion)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    const auto ipv6 = network::SocketAddr::FromIPPort("::1", 12345);
    const uint8_t value = 1;
    std::atomic<int> completed{0};
    ASSERT_EQ(adapter->SendDatagramWithCompletion(ipv6, &value, 1, {},
        [&] { ++completed; }), DatagramSendResult::Ok);
    ResumeNetwork();
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send({2}), DatagramSendResult::Ok);
    adapter->Close();
    ResumeNetwork();
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Failed)], 1u);
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Closed)], 1u);
    EXPECT_EQ(stats.sent_packets, 0u);
    EXPECT_EQ(stats.pending_packets, 0u);
    EXPECT_EQ(completed.load(), 0);
}

TEST_F(UdpSendQueueTest, CallbackExceptionsDoNotCancelOtherPacketsOrUndoSocketSuccess)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    const uint8_t value = 7;
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, &value, 1,
        []() -> bool { throw std::runtime_error("validity"); }, {}), DatagramSendResult::Ok);
    ASSERT_EQ(adapter->SendDatagramWithCompletion(remote, &value, 1, {},
        [] { throw std::runtime_error("completion"); }), DatagramSendResult::Ok);
    ASSERT_EQ(Send({8}), DatagramSendResult::Ok);
    ResumeNetwork();
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{7}));
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{8}));
    EXPECT_TRUE(Receive(20).empty());
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.callback_errors, 2u);
    EXPECT_EQ(stats.sent_packets, 2u);
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::CallbackError)], 1u);
    EXPECT_EQ(stats.pending_packets, 0u);
    EXPECT_EQ(budget->Used().second, 0u);
}

TEST_F(UdpSendQueueTest, AdmissionFailuresAreNotCountedAsAcceptedPacketDrops)
{
    CreateQueue({1024, 1});
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send({1}), DatagramSendResult::Ok);
    EXPECT_EQ(Send({2}), DatagramSendResult::NotWritable);
    auto malformed = remote;
    malformed.len = sizeof(sockaddr_storage);
    const uint8_t value = 3;
    EXPECT_EQ(queue->Send(malformed, &value, 1, adapter), DatagramSendResult::Failed);
    ResumeNetwork();
    EXPECT_EQ(Receive(), (std::vector<uint8_t>{1}));
    owner->Invoke([] {});
    const auto stats = queue->GetStats();
    EXPECT_EQ(stats.enqueued, 1u);
    EXPECT_EQ(stats.sent_packets, 1u);
    EXPECT_EQ(stats.rejected_full, 1u);
    EXPECT_EQ(stats.rejected_invalid, 1u);
    for (const auto count : stats.drops) EXPECT_EQ(count, 0u);
}

TEST_F(UdpSendQueueTest, QueueRequiresTheSocketsExecutor)
{
    auto wrong_owner = std::make_shared<TaskScheduler>();
    EXPECT_THROW(std::make_shared<UdpSendQueue>(wrong_owner, server, budget), std::invalid_argument);
}

TEST_F(UdpSendQueueTest, ConcurrentAdmissionAndCancellationAccountForEveryAcceptedPacket)
{
    CreateQueue();
    ASSERT_TRUE(PauseNetwork());
    ASSERT_EQ(Send({1}), DatagramSendResult::Ok);
    std::atomic<uint64_t> accepted{1}, rejected{0};
    std::promise<void> start;
    const auto gate = start.get_future().share();
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i)
        producers.emplace_back([&] {
            gate.wait();
            for (int n = 0; n < 64; ++n)
            {
                const auto result = Send({2});
                if (result == DatagramSendResult::Ok) ++accepted;
                else { EXPECT_EQ(result, DatagramSendResult::Closed); ++rejected; }
            }
        });
    start.set_value();
    queue->Cancel();
    for (auto& producer : producers) producer.join();
    ResumeNetwork();
    const auto stats = queue->GetStats();
    EXPECT_EQ(accepted.load() + rejected.load(), 257u);
    EXPECT_EQ(stats.enqueued, accepted.load());
    EXPECT_EQ(stats.rejected_closed, rejected.load());
    EXPECT_EQ(stats.drops[static_cast<size_t>(monitoring::SendDropReason::Cancelled)], accepted.load());
    EXPECT_EQ(stats.sent_packets, 0u);
    EXPECT_EQ(stats.pending_packets, 0u);
    EXPECT_EQ(stats.pending_bytes, 0u);
    EXPECT_EQ(budget->Used().first, 0u);
    EXPECT_EQ(budget->Used().second, 0u);
    EXPECT_TRUE(Receive(20).empty());
}
}
