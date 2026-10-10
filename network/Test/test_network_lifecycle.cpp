#include "TcpConnection.h"
#include "TcpServer.h"
#include "TcpSession.h"
#include "UdpServer.h"
#include "transport/UdpDatagramTransport.h"
#if defined(__APPLE__)
#include "KqueueTaskScheduler.h"
using NetworkScheduler = KqueueTaskScheduler;
#else
#include "EpollTaskScheduler.h"
using NetworkScheduler = EpollTaskScheduler;
#endif
#include "rtmp_transport.h"
#include "rtmp_server.h"

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
    auto scheduler = std::make_shared<NetworkScheduler>();
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

class SaturatedScheduler : public NetworkScheduler {
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

TEST(NetworkLifecycle, ReceiveBufferCompactsAndReportsItsLimitWithoutFakingEof) {
    SocketPair sockets;
    BufferReader buffer(2048, 32);
    ASSERT_EQ(buffer.Size(), 32);
    const std::string initial(32, 'a');
    ASSERT_EQ(::send(sockets.fd[1], initial.data(), initial.size(), 0), 32);
    ASSERT_EQ(buffer.Read(sockets.fd[0]), 32);
    ASSERT_EQ(::send(sockets.fd[1], "bbbbbbbbbbbbbbbb", 16, 0), 16);
    buffer.Retrieve(16);
    ASSERT_EQ(buffer.Read(sockets.fd[0]), 16);
    EXPECT_EQ(std::string(buffer.Peek(), buffer.ReadableBytes()),
              std::string(16, 'a') + std::string(16, 'b'));
    EXPECT_EQ(buffer.Size(), 32);
    EXPECT_EQ(buffer.Read(sockets.fd[0]), -1);
    EXPECT_EQ(errno, EMSGSIZE);
    buffer.RetrieveAll();
    ASSERT_EQ(::send(sockets.fd[1], "z", 1, 0), 1);
    EXPECT_EQ(buffer.Read(sockets.fd[0]), 1);
    EXPECT_EQ(*buffer.Peek(), 'z');
    EXPECT_EQ(buffer.Size(), 32);
}

TEST(NetworkLifecycle, TcpReadBudgetDrainsAcrossEventsAndFlushesAfterHalfClose) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    TcpConnection::Options options;
    options.max_receive_bytes = 32;
    options.read_budget_bytes = 16;
    options.max_send_bytes = 64;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst(), options);
    std::string received;
    size_t largest_batch = 0;
    bool replied = false;
    std::promise<void> closed;
    auto done = closed.get_future();
    connection->SetReadCallback([&](auto c, BufferReader& buffer) {
        largest_batch = std::max(largest_batch, static_cast<size_t>(buffer.ReadableBytes()));
        received.append(buffer.Peek(), buffer.ReadableBytes());
        buffer.RetrieveAll();
        if (received.size() == 128 && !replied) {
            replied = true;
            return c->Send("ok", 2) == TcpConnection::SendResult::Queued;
        }
        return true;
    });
    connection->SetCloseCallback(TcpConnection::CloseCallback([&](auto) { closed.set_value(); }));
    const timeval timeout{2, 0};
    ASSERT_EQ(::setsockopt(sockets.fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    const std::string payload(128, 'x');
    ASSERT_EQ(::send(sockets.fd[1], payload.data(), payload.size(), 0), 128);
    ASSERT_EQ(::shutdown(sockets.fd[1], SHUT_WR), 0);
    connection->Start();
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(received, payload);
    EXPECT_LE(largest_batch, 16);
    char response[2]{};
    EXPECT_EQ(::recv(sockets.fd[1], response, 2, MSG_WAITALL), 2);
    EXPECT_EQ(std::string(response, 2), "ok");
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, TcpClosesWhenParserRetainsFullReceiveBuffer) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    TcpConnection::Options options;
    options.max_receive_bytes = 32;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst(), options);
    std::promise<uint32_t> closed;
    auto done = closed.get_future();
    uint32_t retained = 0;
    connection->SetReadCallback([&](auto, BufferReader& buffer) {
        retained = buffer.ReadableBytes();
        EXPECT_LE(buffer.Size(), 32);
        return true;
    });
    connection->SetCloseCallback(TcpConnection::CloseCallback([&](auto) { closed.set_value(retained); }));
    const std::string payload(64, 'x');
    ASSERT_EQ(::send(sockets.fd[1], payload.data(), payload.size(), 0), 64);
    connection->Start();
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(done.get(), 32);
    EXPECT_TRUE(connection->IsClosed());
    connection->Disconnect();
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, TcpFullBufferCanBeConsumedByScheduledParserContinuation) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    TcpConnection::Options options;
    options.max_receive_bytes = 32;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst(), options);
    bool deferred = false;
    std::string received;
    std::promise<void> closed;
    auto done = closed.get_future();
    connection->SetReadCallback([&](auto current, BufferReader& buffer) {
        if (!deferred && buffer.ReadableBytes() == 32) {
            deferred = true;
            return current->RequestReadContinuation();
        }
        received.append(buffer.Peek(), buffer.ReadableBytes());
        buffer.RetrieveAll();
        return true;
    });
    connection->SetCloseCallback(TcpConnection::CloseCallback([&](auto) { closed.set_value(); }));
    const std::string payload(32, 'x');
    ASSERT_EQ(::send(sockets.fd[1], payload.data(), payload.size(), 0), 32);
    ASSERT_EQ(::shutdown(sockets.fd[1], SHUT_WR), 0);
    connection->Start();
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(deferred);
    EXPECT_EQ(received, payload);
    connection.reset();
    loop.Stop();
}

struct ByteCodec : itcp_sess::ICodec<std::string> {
    void Feed(const uint8_t* data, size_t size, std::vector<std::string>& out) override {
        out.emplace_back(reinterpret_cast<const char*>(data), size);
    }
    void Encode(const std::string& message, std::vector<uint8_t>& out) override {
        out.assign(message.begin(), message.end());
    }
};

struct ByteObserver : itcp_sess::ISessionObserver<std::string> {
    std::string received;
    std::promise<void> closed;
    void OnMessage(const std::string& message) override { received += message; }
    void OnSessionClosed(int) override { closed.set_value(); }
};

TEST(NetworkLifecycle, TcpSessionUsesBorrowedBytesCallbackAndConsumesExactCapacity) {
    EventLoop loop(1);
    ASSERT_TRUE(loop.Start());
    auto scheduler = loop.GetTaskScheduler();
    SocketPair sockets;
    TcpConnection::Options options;
    options.max_receive_bytes = 32;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst(), options);
    connection->SetReadCallback([](auto, BufferReader&) {
        ADD_FAILURE() << "The bytes callback should replace the buffered callback";
        return false;
    });
    auto session = std::make_shared<itcp_sess::TcpSession<std::string>>(
        connection, std::make_unique<ByteCodec>());
    auto observer = std::make_shared<ByteObserver>();
    session->AddObserver(observer);
    auto done = observer->closed.get_future();
    const std::string payload(64, 'x');
    ASSERT_EQ(::send(sockets.fd[1], payload.data(), payload.size(), 0), 64);
    ASSERT_EQ(::shutdown(sockets.fd[1], SHUT_WR), 0);
    session->Start();
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(observer->received, payload);
    session.reset();
    connection.reset();
    loop.Stop();
}

TEST(NetworkLifecycle, TcpSendQuotaCanBeConfiguredPerConnection) {
    auto scheduler = std::make_shared<NetworkScheduler>();
    SocketPair sockets;
    TcpConnection::Options options;
    options.max_send_bytes = 8;
    auto connection = std::make_shared<TcpConnection>(scheduler.get(), sockets.TakeFirst(), options);
    EXPECT_EQ(connection->Send("12345678", 8), TcpConnection::SendResult::Queued);
    EXPECT_EQ(connection->Send("x", 1), TcpConnection::SendResult::QueueFull);
    connection->Disconnect();
    EXPECT_EQ(connection->Send("x", 1), TcpConnection::SendResult::Closed);
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
    // macOS defaults SO_SNDBUF to 9216 bytes, which rejects this 16 KB
    // datagram with EMSGSIZE before it reaches the server.
    const int send_buffer = 64 * 1024;
    EXPECT_EQ(::setsockopt(client, SOL_SOCKET, SO_SNDBUF,
                          &send_buffer, sizeof(send_buffer)), 0);
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
        // Closing the server descriptor does not guarantee the TCP FIN has
        // reached the peer yet. Wait for EOF with a bounded receive timeout.
        const timeval timeout{2, 0};
        const int configured = ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                                            &timeout, sizeof(timeout));
        EXPECT_EQ(configured, 0);
        if (configured == 0) {
            char byte;
            ssize_t result;
            do { result = ::recv(client, &byte, 1, 0); }
            while (result < 0 && errno == EINTR);
            EXPECT_EQ(result, 0);
        }
        ::close(client);
        loop.Stop();
    }
}

class RegisteredServer : public TcpServer {
public:
    using TcpServer::TcpServer;
    ~RegisteredServer() override { Stop(); }
    std::promise<bool> initialized;
    std::promise<bool> removed;
protected:
    void OnConnected(const TcpConnection::Ptr& connection) override {
        const bool registered = connections_.count(connection->GetSocket()) == 1;
        initialized.set_value(registered && GetTaskScheduler()->IsCurrentThread());
        connection->Disconnect();
    }
    void RemoveConnection(SOCKET fd) override {
        const bool registered = connections_.count(fd) == 1;
        TcpServer::RemoveConnection(fd);
        removed.set_value(registered && GetTaskScheduler()->IsCurrentThread());
    }
};

TEST(NetworkLifecycle, ExplicitOwnerIsRetainedAndInitializationCloseRemovesConnection) {
    EventLoop loop(2);
    ASSERT_TRUE(loop.Start());
    auto owner = loop.GetTaskScheduler();
    auto other = loop.GetTaskScheduler();
    ASSERT_NE(owner, other);
    RegisteredServer server(owner);
    auto initialized = server.initialized.get_future();
    auto removed = server.removed.get_future();
    auto udp = std::make_shared<network::UdpServer>(owner);
    ASSERT_TRUE(udp->Start("127.0.0.1", 0));
    ASSERT_TRUE(server.Start("127.0.0.1", 0));
    EXPECT_EQ(server.GetTaskScheduler(), owner);
    EXPECT_EQ(udp->GetTaskScheduler(), owner);
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    auto address = network::SocketAddr::FromIPPort("127.0.0.1", server.GetPort());
    const int result = ::connect(client, reinterpret_cast<sockaddr*>(&address.ss), address.len);
    ::close(client);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(initialized.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(initialized.get());
    ASSERT_EQ(removed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(removed.get());
    server.Stop();
    ASSERT_TRUE(server.Start("127.0.0.1", 0));
    EXPECT_EQ(server.GetTaskScheduler(), owner);
    loop.Stop();
    server.Stop();
    udp->Stop();
    EXPECT_FALSE(server.Start("127.0.0.1", 0));
}

TEST(NetworkLifecycle, RtmpHandshakeStartsAfterServerRegistersConnection) {
    EventLoop loop(2);
    ASSERT_TRUE(loop.Start());
    protocol::rtmp::RtmpServer server(&loop);
    ASSERT_TRUE(server.Start("127.0.0.1", 0));
    SocketPair sockets;
    ::close(sockets.fd[0]);
    sockets.fd[0] = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sockets.fd[0], 0);
    const timeval timeout{2, 0};
    ASSERT_EQ(::setsockopt(sockets.fd[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    auto address = network::SocketAddr::FromIPPort("127.0.0.1", server.GetPort());
    ASSERT_EQ(::connect(sockets.fd[0], reinterpret_cast<sockaddr*>(&address.ss), address.len), 0);
    std::vector<uint8_t> request(1537, 0);
    request[0] = 3;
    ASSERT_EQ(::send(sockets.fd[0], request.data(), request.size(), 0), request.size());
    std::vector<uint8_t> response(3073);
    ASSERT_EQ(::recv(sockets.fd[0], response.data(), response.size(), MSG_WAITALL), response.size());
    EXPECT_EQ(response[0], 3);
    EXPECT_TRUE(std::equal(request.begin() + 1, request.end(), response.begin() + 1537));
    server.Stop();
    loop.Stop();
}
} // namespace
