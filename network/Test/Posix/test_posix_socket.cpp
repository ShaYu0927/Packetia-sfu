#include "Socket.h"
#include "SocketUtil.h"
#include "TcpSocket.h"
#include "BufferWrite.h"
#include "Pip.h"

#include <csignal>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* expression, int line)
{
    if (!condition)
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

struct Descriptor {
    int fd;
    ~Descriptor() { if (fd >= 0) ::close(fd); }
};

void CheckDescriptorFlags(int fd)
{
    CHECK((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
}

void TestPipe()
{
    Pip pipe;
    CHECK(pipe.Create());
    CheckDescriptorFlags(pipe.ReadFd());
    CheckDescriptorFlags(pipe.WriteFd());
#ifdef F_GETNOSIGPIPE
    CHECK(::fcntl(pipe.WriteFd(), F_GETNOSIGPIPE) == 1);
#endif
    char value = 0;
    CHECK(pipe.Read(&value, 1) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    value = 'p';
    CHECK(pipe.Write(&value, 1) == 1);
    value = 0;
    CHECK(pipe.Read(&value, 1) == 1);
    CHECK(value == 'p');

    const int oldRead = pipe.ReadFd();
    const int oldWrite = pipe.WriteFd();
    CHECK(pipe.Create());
    CHECK(::fcntl(oldRead, F_GETFD) == -1 && errno == EBADF);
    CHECK(::fcntl(oldWrite, F_GETFD) == -1 && errno == EBADF);
    CheckDescriptorFlags(pipe.ReadFd());
    CheckDescriptorFlags(pipe.WriteFd());
    pipe.Close();
    pipe.Close();
    CHECK(pipe.ReadFd() == -1 && pipe.WriteFd() == -1);
}

void TestSocketFlagsAndTimeout()
{
    Descriptor socket{::socket(AF_INET, SOCK_STREAM, 0)};
    CHECK(socket.fd >= 0);
    CHECK(SocketUtil::SetNonBlock(socket.fd));
    CHECK(SocketUtil::SetCloseOnExec(socket.fd));
    CheckDescriptorFlags(socket.fd);
    CHECK(SocketUtil::SetBlock(socket.fd, 125));
    CHECK((::fcntl(socket.fd, F_GETFL) & O_NONBLOCK) == 0);
    timeval timeout{};
    socklen_t size = sizeof(timeout);
    CHECK(::getsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, &size) == 0);
    CHECK(timeout.tv_sec > 0 || timeout.tv_usec > 0);
    CHECK(SocketUtil::SetNoSigpipe(socket.fd));
#ifdef SO_NOSIGPIPE
    int enabled = 0;
    size = sizeof(enabled);
    CHECK(::getsockopt(socket.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, &size) == 0);
    CHECK(enabled == 1);
#endif
    CHECK(!SocketUtil::SetNonBlock(-1));
    CHECK(!SocketUtil::SetCloseOnExec(-1));
    CHECK(!SocketUtil::SetBlock(-1));
}

void TestAccept()
{
    TcpSocket listener;
    CHECK(listener.Create() >= 0);
    Descriptor listenerOwner{listener.GetSocket()};
    CHECK((::fcntl(listener.GetSocket(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(SocketUtil::SetNonBlock(listener.GetSocket()));
    CHECK(listener.Bind("127.0.0.1", 0));
    CHECK(listener.Listen(8));
    CHECK(listener.Accept() == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    sockaddr_in address{};
    CHECK(SocketUtil::GetSocketAddr(listener.GetSocket(), &address) == 0);
    Descriptor client{::socket(AF_INET, SOCK_STREAM, 0)};
    CHECK(client.fd >= 0);
    CHECK(::connect(client.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    // A successful client connect can precede listener readiness on macOS.
    pollfd readiness{listener.GetSocket(), POLLIN, 0};
    int ready;
    do { ready = ::poll(&readiness, 1, 1000); }
    while (ready < 0 && errno == EINTR);
    CHECK(ready == 1 && (readiness.revents & POLLIN) != 0);
    Descriptor accepted{listener.Accept()};
    CHECK(accepted.fd >= 0);
    CheckDescriptorFlags(accepted.fd);
#ifdef SO_NOSIGPIPE
    int enabled = 0;
    socklen_t size = sizeof(enabled);
    CHECK(::getsockopt(accepted.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, &size) == 0);
    CHECK(enabled == 1);
#endif
}

void TestBackpressureAndPeerClose()
{
    int pair[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Descriptor sender{pair[0]};
    Descriptor receiver{pair[1]};
    CHECK(SocketUtil::SetNonBlock(sender.fd));
    CHECK(SocketUtil::SetNonBlock(receiver.fd));
    CHECK(SocketUtil::SetNoSigpipe(sender.fd));
    SocketUtil::SetSendBufSize(sender.fd, 1024);
    const std::string payload(1024 * 1024, 'x');
    BufferWirte buffer;
    CHECK(buffer.Append(payload.data(), static_cast<uint32_t>(payload.size())));
    CHECK(buffer.Send(sender.fd) == 0);
    CHECK(!buffer.IsEmpty());
    CHECK(buffer.QueuedBytes() < payload.size());

    std::string received;
    std::vector<char> chunk(16384);
    for (int attempt = 0; attempt < 2048; ++attempt)
    {
        for (;;)
        {
            const auto count = ::recv(receiver.fd, chunk.data(), chunk.size(), 0);
            if (count > 0) received.append(chunk.data(), static_cast<std::size_t>(count));
            else
            {
                CHECK(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
                break;
            }
        }
        if (buffer.IsEmpty()) break;
        CHECK(buffer.Send(sender.fd) == 0);
    }
    CHECK(buffer.IsEmpty());
    CHECK(received == payload);

    ::close(receiver.fd);
    receiver.fd = -1;
    CHECK(buffer.Append("closed", 6));
    // main restores SIGPIPE's default disposition. Failure to suppress the
    // signal terminates this test instead of accidentally hiding the bug.
    CHECK(buffer.Send(sender.fd) == -1);
    CHECK(errno == EPIPE || errno == ECONNRESET);
}
}

int main()
{
    std::signal(SIGPIPE, SIG_DFL);
    try
    {
        TestPipe();
        TestSocketFlagsAndTimeout();
        TestAccept();
        TestBackpressureAndPeerClose();
        std::cout << "POSIX socket and pipe tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
