#include "TcpConnection.h"
#include "TcpServer.h"
#include "UdpServer.h"
#include "transport/UdpDatagramTransport.h"
#include "EpollTaskScheduler.h"
#include "rtmp_transport.h"

#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

struct SocketPair {
    int fd[2]{-1, -1};
    SocketPair() { if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) std::abort(); }
    ~SocketPair() { for (int s : fd) if (s >= 0) ::close(s); }
    int TakeFirst() { int s = fd[0]; fd[0] = -1; return s; }
};

class ObservedConnection : public TcpConnection {
public:
    using TcpConnection::TcpConnection;
    std::atomic<bool> wrote_on_owner{false};
protected:
    void HandleWrite() override {
        wrote_on_owner = GetTaskScheduler()->IsCurrentThread();
        TcpConnection::HandleWrite();
    }
};

TEST(NetworkLifecycle, TcpAdmissionIsBoundedAndRtmpSeesBackpressure) {
    // A scheduler not yet running keeps accepted bytes queued deterministically.
    auto scheduler = std::make_shared<EpollTaskScheduler>();
    SocketPair sockets;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst());
    auto transport = std::make_shared<protocol::rtmp::RtmpTcpTransport>(connection);
    ASSERT_TRUE(transport->Start({}));
    std::vector<char> payload(BufferWirte::KDefaultMaxQueuedBytes, 'x');
    EXPECT_EQ(connection->Send(payload.data(), payload.size()), TcpConnection::SendResult::Queued);
    EXPECT_EQ(connection->Send("x", 1), TcpConnection::SendResult::QueueFull);
    EXPECT_EQ(transport->Send(reinterpret_cast<const uint8_t*>("x"), 1),
              protocol::rtmp::RtmpTransportSendResult::NotWritable);
    connection->close();
    EXPECT_EQ(connection->Send("x", 1), TcpConnection::SendResult::Closed);
}

TEST(NetworkLifecycle, TcpWritesOnOwnerAndClosesExactlyOnceWithoutCallbackLock) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    auto connection = std::make_shared<ObservedConnection>(scheduler.get(), sockets.TakeFirst());
    int closed = 0;
    int session_closed = 0;
    connection->SetCloseCallback(TcpConnection::CloseCallback([&](auto c) {
        EXPECT_TRUE(scheduler->IsCurrentThread());
        EXPECT_EQ(c->Send("ignored", 7), TcpConnection::SendResult::Closed);
        c->Disconnect(); // Reentrant close must neither deadlock nor notify twice.
        ++closed;
    }));
    connection->SetCloseCallback(TcpConnection::SessionCloseCallback([&](int) { ++session_closed; }));
    connection->Start();
    EXPECT_EQ(connection->Send("hello", 5), TcpConnection::SendResult::Queued);
    scheduler->Invoke([] {});
    EXPECT_TRUE(connection->wrote_on_owner.load());
    char data[5]{};
    EXPECT_EQ(::recv(sockets.fd[1], data, sizeof(data), MSG_DONTWAIT), 5);
    EXPECT_EQ(std::string(data, 5), "hello");
    connection->Disconnect();
    connection->Disconnect();
    EXPECT_EQ(closed, 1);
    EXPECT_EQ(session_closed, 1);
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, TcpPartialWritesPreserveAllAcceptedBytes) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst());
    connection->Start();
    std::vector<char> payload(3 * 1024 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
    ASSERT_EQ(connection->Send(payload.data(), payload.size()), TcpConnection::SendResult::Queued);
    timeval timeout{2, 0};
    ::setsockopt(sockets.fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::vector<char> received(payload.size());
    size_t total = 0;
    while (total < received.size()) {
        const auto size = ::recv(sockets.fd[1], received.data() + total, received.size() - total, 0);
        if (size <= 0) break;
        total += size;
    }
    EXPECT_EQ(total, payload.size());
    EXPECT_EQ(received, payload);
    connection->close();
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, TcpWriteToClosedPeerDoesNotRaiseSigpipe) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst());
    ::close(sockets.fd[1]);
    sockets.fd[1] = -1;
    // No read registration: exercise the write error path directly.
    ASSERT_EQ(connection->Send("x", 1), TcpConnection::SendResult::Queued);
    scheduler->Invoke([] {});
    EXPECT_TRUE(connection->IsClosed());
    connection.reset();
    loop.Stop();
}

class SaturatedScheduler : public EpollTaskScheduler {
public:
    void FillWakeupPipe() {
        char bytes[4096]{};
        while (wakeup_pipe_->Write(bytes, sizeof(bytes)) > 0) {}
    }
};

TEST(NetworkLifecycle, FullWakeupPipeStillReportsAnAcceptedTask) {
    SaturatedScheduler scheduler;
    scheduler.FillWakeupPipe();
    int calls = 0;
    EXPECT_TRUE(scheduler.Post([&] { ++calls; }));
    scheduler.HandleEvent(0);
    EXPECT_EQ(calls, 1);
}

TEST(NetworkLifecycle, TcpHalfCloseFlushesQueuedResponse) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst());
    connection->SetReadCallback([](auto c, BufferReader& buffer) {
        buffer.Retrieve(buffer.ReadableBytes());
        return c->Send("response", 8) == TcpConnection::SendResult::Queued;
    });
    ASSERT_EQ(::send(sockets.fd[1], "request", 7, 0), 7);
    ASSERT_EQ(::shutdown(sockets.fd[1], SHUT_WR), 0);
    connection->Start();
    timeval timeout{2, 0};
    ::setsockopt(sockets.fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char bytes[8]{};
    EXPECT_EQ(::recv(sockets.fd[1], bytes, 8, MSG_WAITALL), 8);
    EXPECT_EQ(std::string(bytes, 8), "response");
    connection->close();
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, StopDrainsAcceptedTasksAndRejectsNewTasks) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    std::atomic<int> completed{0};
    for (int i = 0; i < 1000; ++i) ASSERT_TRUE(scheduler->Post([&] { ++completed; }));
    loop.Stop();
    EXPECT_EQ(completed, 1000);
    EXPECT_FALSE(scheduler->Post([] {}));
    EXPECT_FALSE(scheduler->AddTriggerEvent([] {}));
    bool cleaned = false;
    scheduler->Invoke([&] { cleaned = true; });
    EXPECT_TRUE(cleaned);
    ASSERT_TRUE(loop.Start());
    EXPECT_NE(loop.GetTaskScheduler(), scheduler);
    loop.Stop();
}

struct UdpCollector : network::IUdpHandler {
    std::promise<std::vector<uint8_t>> received;
    int closed = 0;
    void OnDatagram(const network::SocketAddr&, const uint8_t* data, size_t size) override {
        received.set_value(std::vector<uint8_t>(data, data + size));
    }
    void OnClosed(int) override { ++closed; }
};

TEST(NetworkLifecycle, UdpReceivesLargeDatagramsAndDestructionCompletesCleanup) {
    EventLoop loop(2);
    ASSERT_TRUE(loop.Start());
    auto server = std::make_shared<network::UdpServer>(&loop);
    auto handler = std::make_shared<UdpCollector>();
    auto received = handler->received.get_future();
    server->SetHandler(handler);
    ASSERT_TRUE(server->Start("127.0.0.1", 0));
    auto transport = std::make_shared<network::transport::UdpDatagramTransport>(1, server);
    EXPECT_TRUE(transport->IsWritable());
    sockaddr_in address{};
    socklen_t address_size = sizeof(address);
    ASSERT_EQ(::getsockname(server->Fd(), reinterpret_cast<sockaddr*>(&address), &address_size), 0);
    const int client = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(client, 0);
    std::vector<uint8_t> payload(16000, 0xA5);
    EXPECT_EQ(::sendto(client, payload.data(), payload.size(), 0,
                      reinterpret_cast<sockaddr*>(&address), address_size), payload.size());
    ::close(client);
    ASSERT_EQ(received.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(received.get(), payload);
    server->Stop();
    EXPECT_FALSE(transport->IsWritable());
    EXPECT_EQ(handler->closed, 1);
    ASSERT_TRUE(server->Start("127.0.0.1", 0));
    server.reset();
    EXPECT_EQ(handler->closed, 2);
    loop.Stop();
}

TEST(NetworkLifecycle, UdpCanBeDestroyedAfterEventLoopStops) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto server = std::make_unique<network::UdpServer>(&loop);
    auto handler = std::make_shared<UdpCollector>();
    server->SetHandler(handler);
    ASSERT_TRUE(server->Start("127.0.0.1", 0));
    loop.Stop();
    server.reset();
    EXPECT_EQ(handler->closed, 1);
}

TEST(NetworkLifecycle, TcpServerStopWorksOnOwnerAndAfterLoopStop) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    TcpServer server(&loop);
    ASSERT_TRUE(server.Start("127.0.0.1", 0));
    loop.GetTaskScheduler()->Invoke([&] { server.Stop(); });
    ASSERT_TRUE(server.Start("127.0.0.1", 0));
    loop.Stop();
    server.Stop();
}

class ObservedServer : public TcpServer {
public:
    using TcpServer::TcpServer;
    ~ObservedServer() override { Stop(); }
    std::promise<void> connected;
protected:
    TcpConnection::Ptr OnConnect(SOCKET fd) override {
        auto connection = TcpServer::OnConnect(fd);
        connected.set_value();
        return connection;
    }
};

TEST(NetworkLifecycle, TcpServerStopsActiveConnectionsOnOwnerAndAfterLoopStop) {
    for (bool stop_loop_first : {false, true}) {
        EventLoop loop(2);
        ASSERT_TRUE(loop.Start());
        ObservedServer server(&loop);
        auto connected = server.connected.get_future();
        ASSERT_TRUE(server.Start("127.0.0.1", 0));
        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        auto address = network::SocketAddr::FromIPPort("127.0.0.1", server.GetPort());
        ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&address.ss), address.len), 0);
        ASSERT_EQ(connected.wait_for(2s), std::future_status::ready);
        // GetTaskScheduler cycles back to the acceptor's scheduler here.
        loop.GetTaskScheduler();
        auto owner = loop.GetTaskScheduler();
        if (stop_loop_first) { loop.Stop(); server.Stop(); }
        else owner->Invoke([&] { server.Stop(); });
        char byte;
        EXPECT_EQ(::recv(client, &byte, 1, MSG_DONTWAIT), 0);
        ::close(client);
        loop.Stop();
    }
}
} // namespace
