#include "WsServer.h"
#include "EventLoop.h"
#include "TaskScheduler.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using network::websocket::WsConnectionInfo;
using network::websocket::WsServer;
using network::websocket::WsServerOptions;

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error(std::string(__FILE__) + ":" + \
        std::to_string(__LINE__) + ": " #condition); \
} while (false)

template <typename Predicate>
void WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int Get() const { return fd_; }
    void Close() {
        if (fd_ < 0) return;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};

uint16_t AvailablePort()
{
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(socket.Get() >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(socket.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    CHECK(::getsockname(socket.Get(), reinterpret_cast<sockaddr*>(&address), &size) == 0);
    return ntohs(address.sin_port);
}

// An independent RFC 6455 client: no libwebsockets client API and no external
// WebSocket dependency. All client frames are masked; server frames are not.
class Client {
public:
    struct Frame {
        uint8_t opcode = 0;
        bool final = false;
        std::string payload;
    };

    explicit Client(uint16_t port) : socket_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        CHECK(socket_.Get() >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        CHECK(::connect(socket_.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    }

    void Close() { socket_.Close(); }

    void LimitReceiveBuffer(int bytes)
    {
        CHECK(::setsockopt(socket_.Get(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0);
    }

    bool Handshake(const std::string& path = "/integration")
    {
        if (!TrySendBytes("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: packetia\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"))
            return false;
        std::string response;
        const auto deadline = Clock::now() + 5s;
        while (response.find("\r\n\r\n") == std::string::npos) {
            char byte;
            if (!ReadExact(&byte, 1, deadline)) return false;
            response.push_back(byte);
            CHECK(response.size() <= 16384);
        }
        if (response.find(" 101 ") == std::string::npos) return false;
        CHECK(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        return true;
    }

    void SendFrame(uint8_t opcode, const std::string& payload, bool final = true)
    {
        std::string bytes;
        bytes.push_back(static_cast<char>((final ? 0x80 : 0) | opcode));
        if (payload.size() < 126) {
            bytes.push_back(static_cast<char>(0x80 | payload.size()));
        } else if (payload.size() <= 65535) {
            bytes.push_back(static_cast<char>(0x80 | 126));
            bytes.push_back(static_cast<char>(payload.size() >> 8));
            bytes.push_back(static_cast<char>(payload.size()));
        } else {
            bytes.push_back(static_cast<char>(0x80 | 127));
            for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<char>(uint64_t(payload.size()) >> shift));
        }
        constexpr std::array<uint8_t, 4> mask{{0x12, 0x34, 0x56, 0x78}};
        for (const auto byte : mask) bytes.push_back(static_cast<char>(byte));
        for (size_t i = 0; i < payload.size(); ++i)
            bytes.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
        SendBytes(bytes);
    }

    bool ReadFrame(Frame& frame)
    {
        const auto deadline = Clock::now() + 5s;
        uint8_t header[2]{};
        if (!ReadExact(header, sizeof(header), deadline)) return false;
        CHECK((header[0] & 0x70) == 0); // No extensions were negotiated.
        CHECK((header[1] & 0x80) == 0);
        frame.final = (header[0] & 0x80) != 0;
        frame.opcode = header[0] & 0x0f;
        uint64_t size = header[1] & 0x7f;
        if (size == 126 || size == 127) {
            const size_t count = size == 126 ? 2 : 8;
            uint8_t extended[8]{};
            CHECK(ReadExact(extended, count, deadline));
            size = 0;
            for (size_t i = 0; i < count; ++i) size = (size << 8) | extended[i];
        }
        CHECK(size <= 4 * 1024 * 1024);
        frame.payload.resize(static_cast<size_t>(size));
        CHECK(ReadExact(frame.payload.data(), frame.payload.size(), deadline));
        return true;
    }

    std::string ReadText()
    {
        std::string message;
        bool started = false;
        for (;;) {
            Frame frame;
            CHECK(ReadFrame(frame));
            if (frame.opcode == 9) {
                SendFrame(10, frame.payload);
                continue;
            }
            if (frame.opcode == 10) continue;
            CHECK(frame.opcode == (started ? 0 : 1));
            started = true;
            message += frame.payload;
            CHECK(message.size() <= 4 * 1024 * 1024);
            if (frame.final) return message;
        }
    }

    void ExpectClosed(uint16_t expected_code = 0)
    {
        Frame frame;
        while (ReadFrame(frame)) {
            if (frame.opcode == 9) { SendFrame(10, frame.payload); continue; }
            CHECK(frame.opcode == 8);
            if (expected_code) {
                CHECK(frame.payload.size() >= 2);
                CHECK((uint16_t(static_cast<uint8_t>(frame.payload[0])) << 8 |
                       static_cast<uint8_t>(frame.payload[1])) == expected_code);
            }
            Close();
            return;
        }
        // Limit rejection is permitted before the upgrade is established.
        CHECK(expected_code == 0);
        Close();
    }

private:
    bool ReadExact(void* destination, size_t size, Clock::time_point deadline)
    {
        auto* output = static_cast<uint8_t*>(destination);
        size_t offset = 0;
        while (offset < size) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            CHECK(remaining.count() > 0);
            pollfd event{socket_.Get(), POLLIN, 0};
            const int result = ::poll(&event, 1, static_cast<int>(remaining.count()));
            if (result < 0 && errno == EINTR) continue;
            CHECK(result > 0);
            const auto received = ::recv(socket_.Get(), output + offset, size - offset, 0);
            if (received < 0 && errno == EINTR) continue;
            if (received == 0 || (received < 0 && errno == ECONNRESET)) return false;
            CHECK(received > 0);
            offset += static_cast<size_t>(received);
        }
        return true;
    }

    void SendBytes(const std::string& bytes)
    {
        CHECK(TrySendBytes(bytes));
    }

    bool TrySendBytes(const std::string& bytes)
    {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::send(socket_.Get(), bytes.data() + offset,
                                        bytes.size() - offset, MSG_NOSIGNAL);
            if (written < 0 && errno == EINTR) continue;
            // Admission can close a newly accepted TCP socket before the
            // upgrade request is written. Only that transport closure is a
            // failed handshake; malformed HTTP/WS assertions still propagate.
            if (written == 0 || (written < 0 && (errno == EPIPE || errno == ECONNRESET)))
                return false;
            CHECK(written > 0);
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    Socket socket_;
};

struct Fixture {
    EventLoop loop{1};
    std::unique_ptr<WsServer> server;
    uint16_t port = 0;
    mutable std::mutex mutex;
    std::vector<WsConnectionInfo> opened;
    std::atomic<size_t> closed{0};

    explicit Fixture(WsServerOptions options = {})
    {
        CHECK(loop.Start());
        server = std::make_unique<WsServer>(&loop, options);
        server->SetOnOpen([this](const auto& info) {
            std::lock_guard<std::mutex> lock(mutex);
            opened.push_back(info);
        });
        server->SetOnClose([this](const auto&) { ++closed; });
        server->SetOnMessage([](const auto&, const auto& message) {
            return message.empty() ? std::string("empty-message") : message;
        });
        Restart();
    }

    ~Fixture() { server->Stop(); loop.Stop(); }

    void Restart()
    {
        // The port is released before lws binds; retry only a bind collision.
        for (int attempt = 0; attempt < 5; ++attempt) {
            port = AvailablePort();
            if (server->Start("127.0.0.1", port)) return;
        }
        CHECK(false);
    }

    size_t OpenCount() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return opened.size();
    }

    std::string Id(size_t index) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(index < opened.size());
        return opened[index].connId;
    }

    std::unique_ptr<Client> Connect()
    {
        const auto previous = OpenCount();
        auto client = std::make_unique<Client>(port);
        CHECK(client->Handshake());
        WaitUntil([&] { return OpenCount() == previous + 1; });
        return client;
    }
};

// Keep the I/O owner paused while checking exact admission bounds. The release
// guard is destroyed before Fixture on every exception path, avoiding deadlock.
class PausedOwner {
public:
    explicit PausedOwner(const std::shared_ptr<TaskScheduler>& scheduler)
    {
        auto entered = std::make_shared<std::promise<void>>();
        auto ready = entered->get_future();
        auto release = release_.get_future().share();
        CHECK(scheduler->Post([entered, release] { entered->set_value(); release.wait(); }));
        CHECK(ready.wait_for(5s) == std::future_status::ready);
    }
    ~PausedOwner() { Release(); }
    void Release() { if (!released_) { released_ = true; release_.set_value(); } }
private:
    std::promise<void> release_;
    bool released_ = false;
};

void HandshakeFramesAndPing()
{
    Fixture fixture;
    auto client = fixture.Connect();
    {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        CHECK(fixture.opened[0].path == "/integration");
        CHECK(!fixture.opened[0].peerAddr.empty());
    }
    client->SendFrame(1, "hello");
    CHECK(client->ReadText() == "hello");
    client->SendFrame(1, "fragment-", false);
    client->SendFrame(9, "probe"); // Control frames may interrupt a fragmented message.
    Client::Frame pong;
    CHECK(client->ReadFrame(pong));
    CHECK(pong.opcode == 10 && pong.final && pong.payload == "probe");
    client->SendFrame(0, "done");
    CHECK(client->ReadText() == "fragment-done");
    client->SendFrame(1, "");
    CHECK(client->ReadText() == "empty-message");
    CHECK(fixture.server->SendText(fixture.Id(0), ""));
    CHECK(client->ReadText().empty());
    const std::string large(70000, 'z'); // Covers 64-bit client length and server continuation frames.
    client->SendFrame(1, large);
    CHECK(client->ReadText() == large);
    WaitUntil([&] { return fixture.server->GetStats().sent_messages == 5; });
    const auto stats = fixture.server->GetStats();
    CHECK(stats.received_messages == 4);
    CHECK(stats.active_connections == 1);
    CHECK(fixture.server->CloseConnection(fixture.Id(0)));
    client->ExpectClosed();
    WaitUntil([&] { return fixture.closed == 1; });
    CHECK(!fixture.server->SendText(fixture.Id(0), "late"));
    CHECK(!fixture.server->CloseConnection(fixture.Id(0)));
}

void OversizedFragmentedMessageIsRejected()
{
    WsServerOptions options;
    options.limits.max_message_bytes = 64;
    Fixture fixture(options);
    auto client = fixture.Connect();
    client->SendFrame(1, std::string(64, 'a'));
    CHECK(client->ReadText() == std::string(64, 'a'));
    CHECK(!fixture.server->SendText(fixture.Id(0), std::string(65, 'b')));
    client->SendFrame(1, std::string(40, 'c'), false);
    client->SendFrame(0, std::string(25, 'd'));
    client->ExpectClosed(1009);
    WaitUntil([&] { return fixture.closed == 1; });
    CHECK(fixture.server->GetStats().received_messages == 1);
    CHECK(fixture.server->GetStats().active_connections == 0);
    auto replacement = fixture.Connect();
    replacement->SendFrame(1, "still-alive");
    CHECK(replacement->ReadText() == "still-alive");
}

void ConnectionLimitAndRepeatedDisconnects()
{
    WsServerOptions options;
    options.limits.max_connections = 2;
    Fixture fixture(options);
    auto first = fixture.Connect();
    auto second = fixture.Connect();
    Client rejected(fixture.port);
    if (rejected.Handshake()) rejected.ExpectClosed();
    WaitUntil([&] { return fixture.server->GetStats().rejected_connections >= 1; });
    CHECK(fixture.server->GetStats().active_connections == 2);
    CHECK(fixture.OpenCount() == 2);
    first->Close();
    WaitUntil([&] { return fixture.closed == 1; });
    second->SendFrame(1, "unaffected");
    CHECK(second->ReadText() == "unaffected");
    std::set<std::string> ids{fixture.Id(0), fixture.Id(1)};
    for (size_t i = 0; i < 20; ++i) {
        auto next = fixture.Connect();
        CHECK(ids.insert(fixture.Id(i + 2)).second);
        next->SendFrame(1, "round-" + std::to_string(i));
        CHECK(next->ReadText() == "round-" + std::to_string(i));
        next->Close();
        WaitUntil([&] { return fixture.closed == i + 2; });
    }
    second->Close();
    WaitUntil([&] { return fixture.closed == 22; });
    CHECK(fixture.server->GetStats().active_connections == 0);
    CHECK(fixture.server->GetStats().accepted_connections == 22);
}

void PendingHandshakeLimitRecovers()
{
    WsServerOptions options;
    options.max_pending_handshakes = 2;
    Fixture fixture(options);
    Client first_idle(fixture.port);
    Client second_idle(fixture.port);
    Client rejected(fixture.port);
    // The rejection counter is the barrier: no timing assumption about when
    // the owner's poll loop accepts these TCP sockets is necessary.
    WaitUntil([&] { return fixture.server->GetStats().rejected_connections >= 1; });
    rejected.ExpectClosed();
    CHECK(fixture.OpenCount() == 0);
    CHECK(fixture.server->GetStats().active_connections == 0);
    first_idle.Close();
    CHECK(second_idle.Handshake());
    WaitUntil([&] { return fixture.OpenCount() == 1; });
    auto replacement = fixture.Connect();
    replacement->SendFrame(1, "pending-capacity-released");
    CHECK(replacement->ReadText() == "pending-capacity-released");
    CHECK(fixture.server->GetStats().active_connections == 2);
}

void ConcurrentProducersPreserveMessages()
{
    Fixture fixture;
    constexpr size_t client_count = 4;
    constexpr size_t producer_count = 4;
    constexpr size_t messages_per_producer = 30;
    std::vector<std::unique_ptr<Client>> clients;
    for (size_t i = 0; i < client_count; ++i) clients.push_back(fixture.Connect());
    std::atomic<bool> accepted{true};
    std::vector<std::thread> producers;
    for (size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (size_t sequence = 0; sequence < messages_per_producer; ++sequence) {
                const auto message = std::to_string(producer) + ":" + std::to_string(sequence);
                for (size_t client = 0; client < client_count; ++client)
                    if (!fixture.server->SendText(fixture.Id(client), message)) accepted = false;
            }
        });
    }
    for (auto& producer : producers) producer.join();
    CHECK(accepted.load());
    for (auto& client : clients) {
        std::set<std::string> received;
        std::array<int, producer_count> last_sequence;
        last_sequence.fill(-1);
        for (size_t i = 0; i < producer_count * messages_per_producer; ++i) {
            const auto message = client->ReadText();
            CHECK(received.insert(message).second);
            const auto split = message.find(':');
            CHECK(split != std::string::npos);
            const auto producer = std::stoul(message.substr(0, split));
            const auto sequence = std::stoi(message.substr(split + 1));
            CHECK(producer < producer_count);
            CHECK(sequence == ++last_sequence[producer]);
        }
        for (const auto sequence : last_sequence)
            CHECK(sequence == static_cast<int>(messages_per_producer) - 1);
    }
    const auto sent = client_count * producer_count * messages_per_producer;
    WaitUntil([&] { return fixture.server->GetStats().sent_messages == sent; });
    WaitUntil([&] { return fixture.server->GetStats().queued_messages == 0; });
    CHECK(fixture.server->GetStats().queued_bytes == 0);
}

void QueueLimitsRecoverAfterDrain()
{
    WsServerOptions options;
    options.limits.max_message_bytes = 128;
    options.limits.max_queued_bytes = 12;
    options.limits.max_queued_messages = 2;
    options.limits.max_total_queued_bytes = 20;
    options.limits.max_total_queued_messages = 3;
    Fixture fixture(options);
    auto first = fixture.Connect();
    auto second = fixture.Connect();
    {
        PausedOwner paused(fixture.loop.GetTaskScheduler());
        CHECK(fixture.server->SendText(fixture.Id(0), "aaaaaa"));
        CHECK(!fixture.server->SendText(fixture.Id(0), "1234567")); // Per-session bytes.
        CHECK(fixture.server->SendText(fixture.Id(0), "bbbbbb"));
        CHECK(!fixture.server->SendText(fixture.Id(0), "")); // Per-session message count.
        CHECK(!fixture.server->SendText(fixture.Id(1), "123456789")); // Global bytes.
        CHECK(fixture.server->SendText(fixture.Id(1), "12345678"));
        CHECK(!fixture.server->SendText(fixture.Id(1), "")); // Global message count.
        const auto stats = fixture.server->GetStats();
        CHECK(stats.queued_bytes == 20);
        CHECK(stats.queued_messages == 3);
        CHECK(stats.rejected_messages >= 4);
    }
    CHECK(first->ReadText() == "aaaaaa");
    CHECK(first->ReadText() == "bbbbbb");
    CHECK(second->ReadText() == "12345678");
    WaitUntil([&] { return fixture.server->GetStats().queued_messages == 0; });
    CHECK(fixture.server->GetStats().queued_bytes == 0);
    CHECK(fixture.server->SendText(fixture.Id(0), "recovered"));
    CHECK(first->ReadText() == "recovered");
}

void OneWritablePerBatchStillServesEveryClient()
{
    WsServerOptions options;
    options.max_writable_batch = 1;
    Fixture fixture(options);
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i) clients.push_back(fixture.Connect());
    {
        PausedOwner paused(fixture.loop.GetTaskScheduler());
        for (size_t client = 0; client < clients.size(); ++client) {
            CHECK(fixture.server->SendText(fixture.Id(client), "first-" + std::to_string(client)));
            CHECK(fixture.server->SendText(fixture.Id(client), "second-" + std::to_string(client)));
        }
    }
    for (size_t client = 0; client < clients.size(); ++client) {
        CHECK(clients[client]->ReadText() == "first-" + std::to_string(client));
        CHECK(clients[client]->ReadText() == "second-" + std::to_string(client));
    }
    WaitUntil([&] { return fixture.server->GetStats().sent_messages == 16; });
    CHECK(fixture.server->GetStats().queued_messages == 0);
}

void SlowReaderDoesNotBlockOtherClients()
{
    WsServerOptions options;
    options.limits.max_message_bytes = 64 * 1024;
    options.limits.max_queued_bytes = 256 * 1024;
    options.limits.max_queued_messages = 8;
    options.limits.max_total_queued_bytes = 1024 * 1024;
    options.limits.max_total_queued_messages = 64;
    Fixture fixture(options);
    auto slow = fixture.Connect();
    slow->LimitReceiveBuffer(4096);
    auto fast = fixture.Connect();
    const auto slow_id = fixture.Id(0);
    std::atomic<bool> run{true};
    std::atomic<size_t> accepted{0}, rejected{0};
    const std::string payload(64 * 1024, 's');
    std::thread producer([&] {
        const auto deadline = Clock::now() + 5s;
        // The slow client never reads application frames. Bound both total
        // attempts and wall time; no assumption about kernel buffer sizes.
        for (size_t attempt = 0; attempt < 4096 && run && Clock::now() < deadline; ++attempt) {
            if (fixture.server->SendText(slow_id, payload)) {
                ++accepted;
            } else {
                ++rejected;
                std::this_thread::sleep_for(1ms);
            }
        }
    });
    struct JoinGuard {
        std::atomic<bool>& run;
        std::thread& thread;
        ~JoinGuard() { run = false; if (thread.joinable()) thread.join(); }
    } guard{run, producer};
    WaitUntil([&] { return rejected > 0; });
    CHECK(accepted > 0);
    for (int sequence = 0; sequence < 5; ++sequence) {
        const auto message = "fast-" + std::to_string(sequence);
        fast->SendFrame(1, message);
        CHECK(fast->ReadText() == message);
    }
    run = false;
    producer.join();
    slow->Close();
    WaitUntil([&] {
        const auto stats = fixture.server->GetStats();
        return fixture.closed == 1 && stats.queued_messages == 0 && stats.queued_bytes == 0;
    });
    CHECK(fixture.server->GetStats().active_connections == 1);
    auto replacement = fixture.Connect();
    replacement->SendFrame(1, "slow-client-replaced");
    CHECK(replacement->ReadText() == "slow-client-replaced");
}

void ThrowingCallbacksCloseOnlyTheAffectedConnection()
{
    Fixture fixture;
    auto first = fixture.Connect();
    auto second = fixture.Connect();
    fixture.server->SetOnMessage([](const auto&, const std::string& message) -> std::string {
        if (message == "throw") throw std::runtime_error("application callback failure");
        return message;
    });
    fixture.server->SetOnClose([&](const auto&) {
        ++fixture.closed;
        throw std::runtime_error("application close callback failure");
    });
    first->SendFrame(1, "throw");
    first->ExpectClosed();
    WaitUntil([&] { return fixture.closed == 1; });
    second->SendFrame(1, "survives");
    CHECK(second->ReadText() == "survives");
    CHECK(fixture.server->GetStats().active_connections == 1);
    fixture.server->Stop();
    CHECK(fixture.closed == 2);
    CHECK(fixture.server->GetStats().active_connections == 0);
    CHECK(fixture.server->GetStats().queued_messages == 0);
    CHECK(fixture.server->GetStats().queued_bytes == 0);
}

void StopFromMessageCallback()
{
    std::atomic<bool> returned{false};
    Fixture fixture;
    auto client = fixture.Connect();
    fixture.server->SetOnMessage([&](const auto&, const auto&) {
        fixture.server->Stop();
        returned = true;
        return std::string{};
    });
    client->SendFrame(1, "stop");
    WaitUntil([&] { return returned.load() && fixture.closed == 1; });
    client->ExpectClosed();
    fixture.server->Stop();
    CHECK(fixture.server->GetStats().active_connections == 0);
    CHECK(fixture.server->GetStats().queued_bytes == 0);
}

void ConcurrentStopAfterLoopStops()
{
    Fixture fixture;
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i) clients.push_back(fixture.Connect());
    fixture.loop.Stop();
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&] { fixture.server->Stop(); });
    for (auto& caller : callers) caller.join();
    CHECK(fixture.closed == 8);
    CHECK(fixture.server->GetStats().active_connections == 0);
    CHECK(fixture.server->GetStats().queued_messages == 0);
    for (auto& client : clients) client->ExpectClosed();
}

void StopFromCloseCallback()
{
    Fixture fixture;
    auto first = fixture.Connect();
    auto second = fixture.Connect();
    fixture.server->SetOnClose([&](const auto&) {
        ++fixture.closed;
        fixture.server->Stop();
    });
    CHECK(fixture.server->CloseConnection(fixture.Id(0)));
    first->ExpectClosed();
    WaitUntil([&] { return fixture.closed == 2; });
    second->ExpectClosed();
    fixture.server->Stop();
    CHECK(fixture.server->GetStats().active_connections == 0);
}

void RestartAfterEventLoopRestart()
{
    Fixture fixture;
    auto first = fixture.Connect();
    const auto first_id = fixture.Id(0);
    fixture.loop.Stop();
    fixture.server->Stop();
    CHECK(fixture.closed == 1);
    CHECK(fixture.loop.Start());
    fixture.Restart();
    auto second = fixture.Connect();
    CHECK(fixture.Id(1) != first_id);
    CHECK(!fixture.server->SendText(first_id, "stale"));
    second->SendFrame(1, "new-owner");
    CHECK(second->ReadText() == "new-owner");
    CHECK(fixture.server->GetStats().accepted_connections == 2);
    CHECK(fixture.server->GetStats().active_connections == 1);
    fixture.server->Stop();
    CHECK(fixture.closed == 2);
    fixture.Restart();
    auto third = fixture.Connect();
    third->SendFrame(1, "same-owner");
    CHECK(third->ReadText() == "same-owner");
}

void RunLoad(size_t client_count, size_t messages_per_client)
{
    CHECK(client_count > 0 && client_count <= 10000);
    CHECK(messages_per_client > 0 && messages_per_client <= 1000000);
    WsServerOptions options;
    options.limits.max_connections = client_count;
    Fixture fixture(options);
    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(client_count);
    const auto connect_start = Clock::now();
    for (size_t i = 0; i < client_count; ++i) clients.push_back(fixture.Connect());
    const double connect_seconds = std::chrono::duration<double>(Clock::now() - connect_start).count();
    std::atomic<size_t> sent{0}, received{0};
    std::mutex error_mutex;
    std::string first_error;
    const size_t group_count = std::min<size_t>(8, client_count);
    std::vector<std::thread> groups;
    const auto traffic_start = Clock::now();
    for (size_t group = 0; group < group_count; ++group) {
        groups.emplace_back([&, group] {
            try {
                for (size_t sequence = 0; sequence < messages_per_client; ++sequence) {
                    // A batch puts one request in flight on every client in
                    // this group before reading any replies from that group.
                    for (size_t client = group; client < client_count; client += group_count) {
                        const auto message = std::to_string(client) + ":" + std::to_string(sequence);
                        clients[client]->SendFrame(1, message);
                        ++sent;
                    }
                    for (size_t client = group; client < client_count; client += group_count) {
                        const auto expected = std::to_string(client) + ":" + std::to_string(sequence);
                        CHECK(clients[client]->ReadText() == expected);
                        ++received;
                    }
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (first_error.empty()) first_error = error.what();
            }
        });
    }
    for (auto& group : groups) group.join();
    const double traffic_seconds = std::chrono::duration<double>(Clock::now() - traffic_start).count();
    const auto expected = client_count * messages_per_client;
    const bool complete = first_error.empty() && sent == expected && received == expected;
    std::cout << "LOAD clients=" << client_count
              << " messages_per_client=" << messages_per_client
              << " sent=" << sent << " received=" << received
              << " missing=" << (sent.load() - received.load())
              << " connect_seconds=" << connect_seconds
              << " send_receive_seconds=" << traffic_seconds
              << " messages_per_second=" << received.load() / traffic_seconds
              << " fifo_and_counts_ok=" << (complete ? "true" : "false") << std::endl;
    if (!first_error.empty()) throw std::runtime_error(first_error);
    CHECK(complete);
    WaitUntil([&] { return fixture.server->GetStats().queued_messages == 0; });
    CHECK(fixture.server->GetStats().sent_messages == expected);
    CHECK(fixture.server->GetStats().received_messages == expected);
    fixture.server->Stop();
    CHECK(fixture.closed == client_count);
    CHECK(fixture.server->GetStats().active_connections == 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 1) {
        if (argc != 4 || std::string(argv[1]) != "--load") {
            std::cerr << "Usage: " << argv[0] << " [--load clients messages-per-client]" << std::endl;
            return 2;
        }
        try {
            RunLoad(std::stoul(argv[2]), std::stoul(argv[3]));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL load: " << error.what() << std::endl;
            return 1;
        }
    }
    const std::pair<const char*, void (*)()> tests[] = {
        {"handshake, fragmented/empty/large messages and ping", HandshakeFramesAndPing},
        {"oversized fragmented message", OversizedFragmentedMessageIsRejected},
        {"connection limit and repeated disconnects", ConnectionLimitAndRepeatedDisconnects},
        {"pending handshake limit recovers", PendingHandshakeLimitRecovers},
        {"concurrent producers and multiple clients", ConcurrentProducersPreserveMessages},
        {"queue limits recover after drain", QueueLimitsRecoverAfterDrain},
        {"one writable per batch serves all clients", OneWritablePerBatchStillServesEveryClient},
        {"slow reader does not block other clients", SlowReaderDoesNotBlockOtherClients},
        {"callback exceptions preserve cleanup", ThrowingCallbacksCloseOnlyTheAffectedConnection},
        {"Stop from message callback", StopFromMessageCallback},
        {"Stop from close callback", StopFromCloseCallback},
        {"concurrent Stop after EventLoop stops", ConcurrentStopAfterLoopStops},
        {"restart after EventLoop restart", RestartAfterEventLoopRestart},
    };
    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "PASS " << test.first << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.first << ": " << error.what() << std::endl;
            return 1;
        }
    }
    std::cout << passed << " WebSocket integration tests passed" << std::endl;
    return 0;
}
