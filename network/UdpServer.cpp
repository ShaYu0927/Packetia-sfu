#include "UdpServer.h"
namespace network
{
UdpServer::UdpServer(EventLoop *loop)
    : event_loop_(loop)
{
}

UdpServer::~UdpServer()
{
    Stop();
}

UdpServer::UdpServer(std::shared_ptr<TaskScheduler> scheduler)
    : scheduler_(std::move(scheduler))
{
}

SocketAddr UdpServer::LocalAddress() const
{
    SocketAddr result;
    if (scheduler_) scheduler_->Invoke([&] {
        if (!started_) return;
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (::getsockname(sock_.Fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0)
            result = SocketAddr::FromSockaddr(reinterpret_cast<sockaddr*>(&address), length);
    });
    return result;
}

void UdpServer::SetHandler(IUdpHandler::Ptr h)
{
    if (scheduler_) scheduler_->Invoke([this, h = std::move(h)] { handler_ = h; });
    else handler_ = std::move(h);
}

bool UdpServer::Start(const std::string &ip, uint16_t port, bool reuse_address)
{
    Stop();

    if (event_loop_) scheduler_ = event_loop_->GetTaskScheduler();
    if (!scheduler_ || scheduler_->IsStopped())
    {
        LOG_ERROR("UdpServer Start: no scheduler");
        return false;
    }

    bool result = false;
    scheduler_->Invoke([&, this] {
    if (sock_.Create() < 0)
    {
        LOG_ERROR("UdpServer Start: socket create failed");
        return;
    }

    if (!sock_.Bind(ip, port, reuse_address))
    {
        LOG_ERROR("UdpServer Start: bind failed");
        sock_.Close();
        return;
    }

    channel_ = std::make_shared<Channel>(sock_.Fd());

    auto weak = weak_from_this();
    if (!weak.expired()) {
        // A media transport may release its last server reference inside a
        // packet callback. Keep the server alive until OnReadable returns.
        channel_->SetReadCallback([weak] {
            if (auto self = weak.lock()) self->OnReadable();
        });
    } else {
        // Stack/unique owners use synchronous Stop before destruction.
        channel_->SetReadCallback([this]() { this->OnReadable(); });
    }
    channel_->EnableReading();

    scheduler_->UpdateChannel(channel_);
    started_ = true;

    LOG_INFO("UdpServer started fd=", sock_.Fd(), " bind=", ip, ":", port);
    result = true;
    });
    return result;
}

void UdpServer::Stop()
{
    auto cleanup = [this]() {
        if (!started_.exchange(false)) return;

        if (channel_) 
        {
            scheduler_->RemoveChannel(channel_);
            channel_.reset();
        }

        sock_.Close();

        auto handler = handler_;
        if (handler) handler->OnClosed(0);
        LOG_INFO("UdpServer stopped");
    };

    // Completion is guaranteed before returning, including during destruction
    // and when the event loop has already stopped.
    if (scheduler_) scheduler_->Invoke(cleanup);
    else cleanup();
}
bool UdpServer::SendTo(const network::SocketAddr &dst, const uint8_t *data, size_t len)
{
    return TrySendTo(dst, data, len) == SendResult::Sent;
}

UdpServer::SendResult UdpServer::TrySendTo(const network::SocketAddr& dst,
                                          const uint8_t* data, size_t len)
{
    if (!data || len == 0 || len > 65507 || dst.len == 0 || dst.len > sizeof(dst.ss))
        return SendResult::Failed;
    if (!scheduler_ || !started_) return SendResult::Closed;
    SendResult result = SendResult::Closed;
    scheduler_->Invoke([&] {
        if (!started_ || scheduler_->IsStopped()) return;
        const int n = sock_.SendTo(dst, data, len);
        result = n == 0 ? SendResult::Sent :
            n == -1 ? SendResult::NotWritable : SendResult::Failed;
    });
    return result;
}
void UdpServer::OnReadable()
{
    // Full IPv4 UDP payload (the socket is AF_INET); no silent 2 KB truncation.
    uint8_t buf[65536];
    for (int i = 0; i < 32; ++i)
    {
        if (!started_) break;
        SocketAddr src;
        int n = sock_.RecvFrom(buf, sizeof(buf), src);
        if (n == -1) break; 
        if (n == -2) 
        {
            LOG_ERROR("UdpServer recv error errno=", errno);
            auto handler = handler_;
            if (handler) handler->OnError(errno);
            break;
        }

        auto handler = handler_;
        if (handler) handler->OnDatagram(src, buf, (size_t)n);
    }
}
}
