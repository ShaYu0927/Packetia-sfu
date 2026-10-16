#include "HttpServer.h"
#include "EventLoop.h"
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace
{
using namespace protocol::http;
using namespace std::chrono_literals;

class Socket
{
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int Get() const { return fd_; }
private:
    int fd_;
};

std::uint16_t Port(int fd)
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length)) return 0;
    return ntohs(address.sin_port);
}

std::uint16_t FreePort()
{
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 0;
    return Port(socket.Get());
}

int Connect(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { close(fd); return -1; }
    return fd;
}

bool Send(int fd, const std::string& value)
{
    std::size_t offset = 0;
    while (offset < value.size())
    {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const auto count = send(fd, value.data() + offset, value.size() - offset, flags);
        if (count <= 0) return false;
        offset += count;
    }
    return true;
}

std::string Response(int fd, bool head = false)
{
    std::string result;
    char buffer[4096];
    while (true)
    {
        const auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) return result;
        result.append(buffer, count);
        const auto header_end = result.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        if (head) return result;
        const auto size_at = result.find("content-length:");
        if (size_at != std::string::npos)
        {
            const auto length = std::stoull(result.substr(size_at + 15));
            if (result.size() >= header_end + 4 + length) return result;
        }
    }
}

std::string Get(const std::string& path = "/test")
{
    return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

class HttpIntegration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(loop.Start());
        options.enabled = true;
        options.listen_port = FreePort();
        ASSERT_NE(options.listen_port, 0);
    }
    EventLoop loop{1};
    HttpServer server{&loop};
    HttpServerOptions options;
};

TEST_F(HttpIntegration, FragmentedPostAndKeepAliveHead)
{
    ASSERT_TRUE(server.Start(options, [](HttpRequest request, IHttpServer::Respond respond) {
        respond({200, request.method + ':' + request.path + ':' + request.body,
                 {{"content-type", "text/plain"}, {"x-test", "yes"}}});
    }));
    Socket client(Connect(options.listen_port));
    ASSERT_GE(client.Get(), 0);
    ASSERT_TRUE(Send(client.Get(), "POST /echo?ignored=yes HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhe"));
    ASSERT_TRUE(Send(client.Get(), "llo"));
    const auto first = Response(client.Get());
    EXPECT_NE(first.find("200 OK"), std::string::npos);
    EXPECT_NE(first.find("POST:/echo:hello"), std::string::npos);
    EXPECT_NE(first.find("x-test: yes"), std::string::npos);
    ASSERT_TRUE(Send(client.Get(), Get("/second")));
    EXPECT_NE(Response(client.Get()).find("GET:/second:"), std::string::npos);
    ASSERT_TRUE(Send(client.Get(), "HEAD /head HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const auto head = Response(client.Get(), true);
    EXPECT_EQ(head.size(), head.find("\r\n\r\n") + 4);
    EXPECT_NE(head.find("content-length: 11"), std::string::npos);
}

TEST_F(HttpIntegration, RepliesFromWorkerAndIgnoresDuplicateReplies)
{
    std::promise<IHttpServer::Respond> captured;
    auto future = captured.get_future();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) { captured.set_value(respond); }));
    Socket client(Connect(options.listen_port));
    ASSERT_TRUE(Send(client.Get(), Get()));
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto respond = future.get();
    auto worker = std::async(std::launch::async, [respond] {
        respond({201, "worker", {{"content-type", "text/plain"}}});
        respond({500, "duplicate", {}});
    });
    worker.get();
    const auto response = Response(client.Get());
    EXPECT_EQ(response.find("HTTP/1.1 201"), 0U) << response;
    EXPECT_NE(response.find("worker"), std::string::npos);
    EXPECT_EQ(response.find("duplicate"), std::string::npos);
}

TEST_F(HttpIntegration, RejectsOversizeAndAmbiguousFramingBeforeBusiness)
{
    std::atomic<int> dispatched{0};
    options.max_body_bytes = 4;
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) {
        ++dispatched; respond({200, "unexpected", {}});
    }));
    const std::vector<std::string> requests{
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello",
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
        "GET / HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer a\r\nAuthorization: Bearer b\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"};
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        Socket client(Connect(options.listen_port));
        ASSERT_TRUE(Send(client.Get(), requests[i]));
        const auto response = Response(client.Get());
        if (i == 0) EXPECT_NE(response.find("413"), std::string::npos);
        else EXPECT_TRUE(response.empty() || response.find("400") != std::string::npos) << response;
    }
    EXPECT_EQ(dispatched.load(), 0);
}

TEST_F(HttpIntegration, DropsRepliesAfterDisconnectAndRestart)
{
    std::promise<IHttpServer::Respond> captured;
    auto future = captured.get_future();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) { captured.set_value(respond); }));
    {
        Socket client(Connect(options.listen_port));
        ASSERT_TRUE(Send(client.Get(), Get()));
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    }
    auto stale = future.get();
    server.Stop();
    server.Stop();
    ASSERT_TRUE(server.Start(options, [](HttpRequest, IHttpServer::Respond respond) { respond({200, "new", {}}); }));
    Socket client(Connect(options.listen_port));
    ASSERT_TRUE(Send(client.Get(), Get()));
    stale({200, "stale", {}});
    EXPECT_NE(Response(client.Get()).find("new"), std::string::npos);
}

TEST_F(HttpIntegration, TimesOutUnansweredRequestAndRejectsLateReply)
{
    options.request_timeout_seconds = 1;
    std::promise<IHttpServer::Respond> captured;
    auto future = captured.get_future();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) { captured.set_value(respond); }));
    Socket client(Connect(options.listen_port));
    ASSERT_TRUE(Send(client.Get(), Get()));
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    char byte;
    EXPECT_EQ(recv(client.Get(), &byte, 1, 0), 0);
    future.get()({200, "too late", {}});
}

TEST_F(HttpIntegration, CapsConnectionsAndRejectsInvalidResponseHeaders)
{
    options.max_connections = 1;
    std::promise<IHttpServer::Respond> captured;
    auto future = captured.get_future();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) { captured.set_value(respond); }));
    Socket first(Connect(options.listen_port));
    ASSERT_TRUE(Send(first.Get(), Get()));
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    Socket second(Connect(options.listen_port));
    if (second.Get() >= 0)
    {
        Send(second.Get(), Get());
        char byte;
        EXPECT_LE(recv(second.Get(), &byte, 1, 0), 0);
    }
    future.get()({200, "bad", {{"x-test", "injected\r\nheader: value"}}});
    EXPECT_TRUE(Response(first.Get()).empty());
}

TEST_F(HttpIntegration, PartialBodyAndIdleConnectionExpire)
{
    options.request_timeout_seconds = 1;
    options.idle_timeout_seconds = 1;
    std::atomic<int> dispatched{0};
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) {
        ++dispatched; respond({200, "ok", {}});
    }));
    Socket partial(Connect(options.listen_port));
    ASSERT_TRUE(Send(partial.Get(), "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\na"));
    Socket idle(Connect(options.listen_port));
    ASSERT_TRUE(Send(idle.Get(), Get()));
    EXPECT_NE(Response(idle.Get()).find("200 OK"), std::string::npos);
    char byte;
    EXPECT_EQ(recv(partial.Get(), &byte, 1, 0), 0);
    EXPECT_EQ(recv(idle.Get(), &byte, 1, 0), 0);
    EXPECT_EQ(dispatched.load(), 1);
}

TEST_F(HttpIntegration, DisconnectReleasesConnectionBudgetAndOldReplyCannotReachNewClient)
{
    options.max_connections = 1;
    std::promise<IHttpServer::Respond> captured;
    auto future = captured.get_future();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest request, IHttpServer::Respond respond) {
        if (request.path == "/old") captured.set_value(respond);
        else respond({200, "new-client", {}});
    }));
    auto owner = loop.GetTaskScheduler();
    {
        Socket old(Connect(options.listen_port));
        ASSERT_TRUE(Send(old.Get(), Get("/old")));
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        // Wait for the peer's FIN to be processed by the library service tick.
        shutdown(old.Get(), SHUT_RDWR);
    }
    auto serviced = std::make_shared<std::promise<void>>();
    auto serviced_future = serviced->get_future();
    owner->AddTimer([serviced] { serviced->set_value(); return false; }, 30);
    ASSERT_EQ(serviced_future.wait_for(2s), std::future_status::ready);
    Socket next(Connect(options.listen_port));
    ASSERT_TRUE(Send(next.Get(), Get()));
    future.get()({200, "stale", {}});
    const auto response = Response(next.Get());
    EXPECT_NE(response.find("new-client"), std::string::npos);
    EXPECT_EQ(response.find("stale"), std::string::npos);
}

TEST_F(HttpIntegration, EnforcesGlobalResponseBudgetWhileOwnerIsBusy)
{
    options.max_response_bytes = 1024;
    options.max_total_response_bytes = 1024;
    std::promise<IHttpServer::Respond> first_reply, second_reply;
    auto first_future = first_reply.get_future();
    auto second_future = second_reply.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    ASSERT_TRUE(server.Start(options, [&](HttpRequest request, IHttpServer::Respond respond) {
        if (request.path == "/first") first_reply.set_value(respond);
        else { second_reply.set_value(respond); released.wait(); }
    }));
    Socket first(Connect(options.listen_port)), second(Connect(options.listen_port));
    ASSERT_TRUE(Send(first.Get(), Get("/first")));
    ASSERT_EQ(first_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(Send(second.Get(), Get("/second")));
    if (second_future.wait_for(2s) != std::future_status::ready)
    {
        release.set_value();
        FAIL() << "second request did not arrive";
    }
    first_future.get()({200, std::string(800, 'a'), {}});
    second_future.get()({200, std::string(800, 'b'), {}});
    release.set_value();
    EXPECT_NE(Response(first.Get()).find(std::string(800, 'a')), std::string::npos);
    EXPECT_TRUE(Response(second.Get()).empty());
}

TEST_F(HttpIntegration, HandlesConcurrentClientsAndMultiChunkResponses)
{
    const std::string body(70000, 'x');
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond respond) { respond({200, body, {}}); }));
    std::vector<std::future<std::string>> clients;
    for (int i = 0; i < 16; ++i)
        clients.push_back(std::async(std::launch::async, [&] {
            Socket client(Connect(options.listen_port));
            if (!Send(client.Get(), Get())) return std::string{};
            return Response(client.Get());
        }));
    for (auto& client : clients) EXPECT_NE(client.get().find(body), std::string::npos);
}

TEST_F(HttpIntegration, StopInsideHandlerAndRebindAfterLoopRestart)
{
    ASSERT_TRUE(server.Start(options, [&](HttpRequest, IHttpServer::Respond) { server.Stop(); }));
    Socket client(Connect(options.listen_port));
    ASSERT_TRUE(Send(client.Get(), Get()));
    EXPECT_TRUE(Response(client.Get()).empty());
    server.Stop();
    loop.Stop();
    ASSERT_TRUE(loop.Start());
    ASSERT_TRUE(server.Start(options, [](HttpRequest, IHttpServer::Respond respond) { respond({200, "restarted", {}}); }));
    Socket next(Connect(options.listen_port));
    ASSERT_TRUE(Send(next.Get(), Get()));
    EXPECT_NE(Response(next.Get()).find("restarted"), std::string::npos);
}

TEST_F(HttpIntegration, FailsCleanlyWhenPortIsOccupied)
{
    Socket occupied(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(occupied.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(occupied.Get(), 4), 0);
    options.listen_port = Port(occupied.Get());
    EXPECT_FALSE(server.Start(options, [](HttpRequest, IHttpServer::Respond) {}));
    server.Stop();
    options.listen_port = FreePort();
    EXPECT_TRUE(server.Start(options, [](HttpRequest, IHttpServer::Respond respond) { respond({200, "ok", {}}); }));
}
}
