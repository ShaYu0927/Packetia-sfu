#include "UdpDatagramTransport.h"

#include "TimeUtil.h"

#include <utility>

namespace network::transport
{

UdpDatagramTransport::UdpDatagramTransport(
    uint64_t id,
    std::weak_ptr<network::UdpServer> server) noexcept
    : id_(id), server_(std::move(server))
{
    closed_.store(id_ == 0 || server_.expired(), std::memory_order_release);
}

uint64_t UdpDatagramTransport::Id() const noexcept
{
    return id_;
}

bool UdpDatagramTransport::IsWritable() const noexcept
{
    auto server = server_.lock();
    return !closed_.load(std::memory_order_acquire) && server && server->IsWritable();
}

DatagramSendResult UdpDatagramTransport::SendDatagram(
    const network::SocketAddr& remote,
    const uint8_t* data,
    size_t size)
{
    if (closed_.load(std::memory_order_acquire))
    {
        return DatagramSendResult::Closed;
    }
    if (!data || size == 0 || remote.len == 0)
    {
        return DatagramSendResult::Failed;
    }

    auto server = server_.lock();
    if (!server)
    {
        closed_.store(true, std::memory_order_release);
        return DatagramSendResult::Closed;
    }
    switch (server->TrySendTo(remote, data, size))
    {
        case network::UdpServer::SendResult::Sent: return DatagramSendResult::Ok;
        case network::UdpServer::SendResult::NotWritable: return DatagramSendResult::NotWritable;
        case network::UdpServer::SendResult::Closed: return DatagramSendResult::Closed;
        default: return DatagramSendResult::Failed;
    }
}

void UdpDatagramTransport::SetDatagramSink(std::weak_ptr<IDatagramSink> sink)
{
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(sink);
}

void UdpDatagramTransport::Close()
{
    if (closed_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink_.reset();
    }
}

void UdpDatagramTransport::OnDatagram(const network::SocketAddr& source,
                                      const uint8_t* data,
                                      size_t size)
{
    if (!IsWritable() || !data || size == 0 || source.len == 0)
    {
        return;
    }

    std::shared_ptr<IDatagramSink> sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_.lock();
    }
    if (sink)
    {
        sink->OnDatagram(ReceivedDatagram(
            id_, Timestamp::NowMs(), source, data, size));
    }
}

void UdpDatagramTransport::OnClosed(int /*reason*/)
{
    Close();
}

void UdpDatagramTransport::OnError(int /*error*/)
{
    Close();
}

} // namespace network::transport
