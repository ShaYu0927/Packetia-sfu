#include "UdpSendQueue.h"
#include "EventLoop.h"

#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <future>
#include <poll.h>
#include <unistd.h>
#include <vector>

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
        queue = std::make_shared<UdpSendQueue>(owner, server, budget, limits);
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
}
