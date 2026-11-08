#include "UdpSendQueue.h"
#include "TaskScheduler.h"

namespace network::transport
{
UdpSendQueue::UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                           std::shared_ptr<utils::ChannelBudget> budget, utils::ChannelLimits limits)
{
    const std::weak_ptr<TaskScheduler> owner = executor;
    auto postToNetwork = [owner](std::function<void()> task) {
        const auto executor = owner.lock();
        if (!executor) return false;
        return executor->Post(std::move(task));
    };
    auto sendDatagram = [socket](PendingDatagram datagram) {
        const auto transport = datagram.transport.lock();
        const auto server = socket.lock();
        if (!transport || !transport->IsWritable() || !server || (datagram.valid && !datagram.valid())) return;
        if (server->TrySendTo(datagram.remote, datagram.payload.Data(), datagram.payload.Size()) == UdpServer::SendResult::Sent && datagram.on_sent)
            datagram.on_sent();
    };
    queue_ = std::make_unique<Queue>(std::move(postToNetwork), std::move(sendDatagram), std::move(budget), limits);
}

UdpDatagramTransport::Sender UdpSendQueue::MakeSender()
{
    const auto weak = weak_from_this();
    return [weak](const SocketAddr& remote, const uint8_t* data, std::size_t size,
                  std::weak_ptr<UdpDatagramTransport> transport, std::function<bool()> valid,
                  std::function<void()> on_sent) {
        const auto queue = weak.lock();
        if (!queue) return DatagramSendResult::Closed;
        return queue->Send(remote, data, size, std::move(transport), std::move(valid), std::move(on_sent));
    };
}

DatagramSendResult UdpSendQueue::Send(const SocketAddr& remote, const uint8_t* data, std::size_t size,
                                     std::weak_ptr<UdpDatagramTransport> transport,
                                     std::function<bool()> valid, std::function<void()> on_sent)
{
    if (queue_->Status() != utils::ChannelResult::Accepted) return DatagramSendResult::Closed;
    const auto adapter = transport.lock();
    if (!adapter || !adapter->IsWritable()) return DatagramSendResult::Closed;
    if (!data || !size || size > 65507 || !remote.len || remote.len > sizeof(remote.ss))
        return DatagramSendResult::Failed;

    try
    {
        auto payload = common::SharedBuffer::TryCopy(data, size);
        if (payload.Empty()) return DatagramSendResult::NotWritable;
        const auto bytes = payload.StorageSize();
        const auto result = queue_->Send({remote, std::move(payload), std::move(transport),
                                          std::move(valid), std::move(on_sent)}, bytes);
        switch (result)
        {
        case utils::ChannelResult::Accepted: return DatagramSendResult::Ok;
        case utils::ChannelResult::Full: return DatagramSendResult::NotWritable;
        default: return DatagramSendResult::Closed;
        }
    }
    catch (const std::bad_alloc&) { return DatagramSendResult::NotWritable; }
}

void UdpSendQueue::Cancel() { queue_->Cancel(); }
}
