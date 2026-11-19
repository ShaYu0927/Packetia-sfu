#pragma once

#include "BoundedChannel.h"
#include "SharedBuffer.h"
#include "UdpDatagramTransport.h"
#include "MetricTypes.h"

class TaskScheduler;

namespace network::transport
{
// Bounded realtime UDP output. Admission is not delivery; socket backpressure
// drops the datagram instead of blocking the network executor or retrying forever.
class UdpSendQueue : public std::enable_shared_from_this<UdpSendQueue>
{
public:
    static constexpr utils::ChannelLimits kDefaultLimits{4 * 1024 * 1024, 4096};
    struct Options
    {
        uint64_t max_queue_age_ms = 100; // Zero disables expiry, not capacity limits.
        // Optional monotonic clock, safe to call from producers and the owner.
        std::function<uint64_t()> clock;
        // Optional backend override, called only on the network executor.
        std::function<UdpServer::SendResult(const SocketAddr&, const uint8_t*, size_t)> send;
    };
    struct Stats
    {
        uint64_t enqueued = 0, sent_packets = 0, sent_bytes = 0;
        uint64_t rejected_full = 0, rejected_closed = 0, rejected_invalid = 0;
        uint64_t callback_errors = 0;
        size_t pending_packets = 0, pending_bytes = 0; // Includes the active callback.
        std::array<uint64_t, static_cast<size_t>(monitoring::SendDropReason::Count)> drops{};
    };

    UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                 std::shared_ptr<utils::ChannelBudget> budget,
                 utils::ChannelLimits limits = kDefaultLimits);
    UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                 std::shared_ptr<utils::ChannelBudget> budget, utils::ChannelLimits limits, Options options);

    UdpDatagramTransport::Sender MakeSender();

    DatagramSendResult Send(const SocketAddr& remote, const uint8_t* data, std::size_t size,
        std::weak_ptr<UdpDatagramTransport> transport, std::function<bool()> valid = {},
        std::function<void()> on_sent = {});
    void Cancel();
    Stats GetStats() const;

private:
    struct Counters;
    struct Accounting;
    struct PendingDatagram
    {
        SocketAddr remote;
        common::SharedBuffer payload;
        std::weak_ptr<UdpDatagramTransport> transport;
        std::function<bool()> valid;
        std::function<void()> on_sent;
        std::shared_ptr<Accounting> accounting;
    };
    using Queue = utils::BoundedChannel<PendingDatagram>;
    std::shared_ptr<Counters> counters_;
    std::function<uint64_t()> clock_;
    std::unique_ptr<Queue> queue_;
};
}
