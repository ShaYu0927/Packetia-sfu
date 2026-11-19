#include "UdpSendQueue.h"
#include "TaskScheduler.h"
#include "RuntimeMetrics.h"
#include "logger.h"
#include <stdexcept>

namespace network::transport
{
struct UdpSendQueue::Counters
{
    mutable std::mutex mutex;
    Stats stats;

    void Reject(uint64_t Stats::*counter)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++(stats.*counter);
        }
        monitoring::RecordSend(monitoring::Transport::Udp, monitoring::SendEvent::Rejected);
    }
};

// Retained by both the producer and queued packet. Destruction classifies every
// admitted packet exactly once, including cancellation and failed scheduling.
struct UdpSendQueue::Accounting
{
    std::shared_ptr<Counters> counters;
    size_t storage_bytes, payload_bytes;
    uint64_t enqueued_ms;
    bool admitted = false, sent = false;
    monitoring::SendDropReason reason = monitoring::SendDropReason::Cancelled;

    Accounting(std::shared_ptr<Counters> counters, size_t storage, size_t payload, uint64_t now)
        : counters(std::move(counters)), storage_bytes(storage), payload_bytes(payload), enqueued_ms(now) {}

    void Accept()
    {
        std::lock_guard<std::mutex> lock(counters->mutex);
        admitted = true;
        ++counters->stats.enqueued;
        ++counters->stats.pending_packets;
        counters->stats.pending_bytes += storage_bytes;
    }

    ~Accounting()
    {
        if (!admitted) return;
        {
            std::lock_guard<std::mutex> lock(counters->mutex);
            auto& stats = counters->stats;
            --stats.pending_packets;
            stats.pending_bytes -= storage_bytes;
            if (sent) { ++stats.sent_packets; stats.sent_bytes += payload_bytes; }
            else ++stats.drops[static_cast<size_t>(reason)];
        }
        if (!sent) monitoring::RecordSendDrop(monitoring::Transport::Udp, reason);
    }
};

UdpSendQueue::UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                           std::shared_ptr<utils::ChannelBudget> budget, utils::ChannelLimits limits)
    : UdpSendQueue(std::move(executor), std::move(socket), std::move(budget), limits, Options{}) {}

UdpSendQueue::UdpSendQueue(std::shared_ptr<TaskScheduler> executor, std::weak_ptr<UdpServer> socket,
                           std::shared_ptr<utils::ChannelBudget> budget, utils::ChannelLimits limits, Options options)
    : counters_(std::make_shared<Counters>())
{
    if (!executor || !budget) throw std::invalid_argument("UDP send queue requires an executor and budget");
    if (const auto server = socket.lock(); server && server->GetTaskScheduler() != executor)
        throw std::invalid_argument("UDP send queue must use the socket's executor");
    if (!options.clock) options.clock = [] { return Timestamp::NowMs(); };
    clock_ = options.clock;
    const std::weak_ptr<TaskScheduler> owner = executor;
    auto postToNetwork = [owner](std::function<void()> task) {
        const auto executor = owner.lock();
        if (!executor) return false;
        return executor->Post(std::move(task));
    };
    auto sendDatagram = [socket, options](PendingDatagram datagram) {
        auto& accounting = *datagram.accounting;
        try
        {
            const auto transport = datagram.transport.lock();
            const auto server = socket.lock();
            if (!transport || !transport->IsWritable() || !server)
            { accounting.reason = monitoring::SendDropReason::Closed; return; }
            const auto now_ms = options.clock();
            if (options.max_queue_age_ms && (now_ms < accounting.enqueued_ms ||
                now_ms - accounting.enqueued_ms >= options.max_queue_age_ms))
            { accounting.reason = monitoring::SendDropReason::Expired; return; }
            if (datagram.valid && !datagram.valid())
            { accounting.reason = monitoring::SendDropReason::Invalidated; return; }

            const auto result = options.send ? options.send(datagram.remote, datagram.payload.Data(), datagram.payload.Size()) :
                server->TrySendTo(datagram.remote, datagram.payload.Data(), datagram.payload.Size());
            switch (result)
            {
            case UdpServer::SendResult::Sent:
                accounting.sent = true;
                if (datagram.on_sent) datagram.on_sent();
                break;
            case UdpServer::SendResult::NotWritable: accounting.reason = monitoring::SendDropReason::Blocked; break;
            case UdpServer::SendResult::Closed: accounting.reason = monitoring::SendDropReason::Closed; break;
            default: accounting.reason = monitoring::SendDropReason::Failed; break;
            }
        }
        catch (...)
        {
            // A faulty completion must not cancel unrelated packets. A packet
            // already accepted by the socket remains a successful send.
            if (!accounting.sent) accounting.reason = monitoring::SendDropReason::CallbackError;
            {
                std::lock_guard<std::mutex> lock(accounting.counters->mutex);
                ++accounting.counters->stats.callback_errors;
            }
            LOG_ERROR("UDP send callback failed");
        }
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
    if (queue_->Status() != utils::ChannelResult::Accepted)
    { counters_->Reject(&Stats::rejected_closed); return DatagramSendResult::Closed; }
    const auto adapter = transport.lock();
    if (!adapter || !adapter->IsWritable())
    { counters_->Reject(&Stats::rejected_closed); return DatagramSendResult::Closed; }
    if (!data || !size || size > 65507 || !remote.IsValid())
    { counters_->Reject(&Stats::rejected_invalid); return DatagramSendResult::Failed; }

    try
    {
        auto payload = common::SharedBuffer::TryCopy(data, size);
        if (payload.Empty())
        { counters_->Reject(&Stats::rejected_full); return DatagramSendResult::NotWritable; }
        const auto bytes = payload.StorageSize();
        auto accounting = std::make_shared<Accounting>(counters_, bytes, size, clock_());
        const auto result = queue_->Send({remote, std::move(payload), std::move(transport),
                                          std::move(valid), std::move(on_sent), accounting}, bytes);
        switch (result)
        {
        case utils::ChannelResult::Accepted: accounting->Accept(); return DatagramSendResult::Ok;
        case utils::ChannelResult::Full:
            counters_->Reject(&Stats::rejected_full); return DatagramSendResult::NotWritable;
        default:
            counters_->Reject(&Stats::rejected_closed); return DatagramSendResult::Closed;
        }
    }
    catch (const std::bad_alloc&)
    { counters_->Reject(&Stats::rejected_full); return DatagramSendResult::NotWritable; }
}

void UdpSendQueue::Cancel() { queue_->Cancel(); }
UdpSendQueue::Stats UdpSendQueue::GetStats() const
{
    std::lock_guard<std::mutex> lock(counters_->mutex);
    return counters_->stats;
}
}
