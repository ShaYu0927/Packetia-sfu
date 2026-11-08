#pragma once

#include "BoundedChannel.h"
#include "SharedBuffer.h"
#include "UdpDatagramTransport.h"

class TaskScheduler;

namespace network::transport
{
// 网络执行器上的有界 UDP 发送出口；入队的数据报持有独立缓冲区。
class UdpSendQueue : public std::enable_shared_from_this<UdpSendQueue>
{
public:
    static constexpr utils::ChannelLimits kDefaultLimits{4 * 1024 * 1024, 4096};

    UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                 std::shared_ptr<utils::ChannelBudget> budget,
                 utils::ChannelLimits limits = kDefaultLimits);

    UdpDatagramTransport::Sender MakeSender();

    DatagramSendResult Send(const SocketAddr& remote, const uint8_t* data, std::size_t size,
        std::weak_ptr<UdpDatagramTransport> transport, std::function<bool()> valid = {},
        std::function<void()> on_sent = {});
    void Cancel();

private:
    struct PendingDatagram
    {
        SocketAddr remote;
        common::SharedBuffer payload;
        std::weak_ptr<UdpDatagramTransport> transport;
        std::function<bool()> valid;
        std::function<void()> on_sent;
    };
    using Queue = utils::BoundedChannel<PendingDatagram>;
    std::unique_ptr<Queue> queue_;
};
}
