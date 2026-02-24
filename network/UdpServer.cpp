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

void UdpServer::SetHandler(IUdpHandler::Ptr h)
{
    handler_ = std::move(h);
}

bool UdpServer::Start(const std::string &ip, uint16_t port)
{
    Stop();

    scheduler_ = event_loop_->GetTaskScheduler();
    if (!scheduler_) 
    {
        LOG_ERROR("UdpServer Start: no scheduler");
        return false;
    }

    if (sock_.Create() < 0) 
    {
        LOG_ERROR("UdpServer Start: socket create failed");
        return false;
    }

    if (!sock_.Bind(ip, port)) 
    {
        LOG_ERROR("UdpServer Start: bind failed");
        sock_.Close();
        return false;
    }

    channel_ = std::make_shared<Channel>(sock_.Fd());

    channel_->SetReadCallback([this]() { this->OnReadable(); });
    channel_->EnableReading();

    event_loop_->UpdateChannel(channel_);
    started_ = true;

    LOG_INFO("UdpServer started fd=", sock_.Fd(), " bind=", ip, ":", port);
    return true;
}

void UdpServer::Stop()
{
    if (!started_) return;

    auto cleanup = [this]() {
        if (!started_) return;
        started_ = false;

        if (channel_) 
        {
            event_loop_->RemoveChannel(channel_);
            channel_.reset();
        }

        sock_.Close();

        if (handler_) handler_->OnClosed(0);
        LOG_INFO("UdpServer stopped");
    };

    if (!scheduler_ || !scheduler_->AddTriggerEvent(cleanup))
    {
        cleanup();
    }
}
bool UdpServer::SendTo(const network::SocketAddr &dst, const uint8_t *data, size_t len)
{
    if (!started_) return false;
    int n = sock_.SendTo(dst, data, len);
    return n == (int)len;
}
void UdpServer::OnReadable()
{
    uint8_t buf[2048];
    for (int i = 0; i < 32; ++i) 
    {
        SocketAddr src;
        int n = sock_.RecvFrom(buf, sizeof(buf), src);
        if (n == -1) break; 
        if (n == -2) 
        {
            if (handler_) handler_->OnError(errno);
            break;
        }

        if (handler_) handler_->OnDatagram(src, buf, (size_t)n);
    }
}
}
