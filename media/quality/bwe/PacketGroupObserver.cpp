#include "PacketGroupObserver.h"

#include <algorithm>

namespace media
{
PacketGroupObservation PacketGroupObserver::OnFeedback(const TransportFeedback& feedback,
                                                       const PacketGroupContext&)
{
    PacketGroupObservation result;

    [[maybe_unused]] const auto packets = SortAndDeduplicate(feedback);

    return result;
}

std::vector<const PacketFeedback*> PacketGroupObserver::SortAndDeduplicate(
    const TransportFeedback& feedback)
{
    std::vector<const PacketFeedback*> packets;
    packets.reserve(feedback.packet_feedbacks.size());
    for (const auto& packet : feedback.packet_feedbacks)
    {
        if (packet.sent_packet.transport_sequence >= 0)
            packets.push_back(&packet);
    }

    std::sort(packets.begin(), packets.end(), [](const auto* lhs, const auto* rhs) {
        const auto& left = lhs->sent_packet;
        const auto& right = rhs->sent_packet;
        if (left.send_time_ms != right.send_time_ms)
            return left.send_time_ms < right.send_time_ms;
        if (left.transport_sequence != right.transport_sequence)
            return left.transport_sequence < right.transport_sequence;
        // 同批重复优先保留已收到的记录，再选择最早的接收时间。
        if (lhs->received != rhs->received)
            return lhs->received;
        return lhs->receive_time_ms < rhs->receive_time_ms;
    });

    std::size_t unique_count = 0;
    for (const auto* packet : packets)
    {
        const int64_t sequence = packet->sent_packet.transport_sequence;
        if (sequence <= newest_sequence_ &&
            static_cast<uint64_t>(newest_sequence_ - sequence) >= kDedupWindowSize)
            continue;

        const uint64_t tag = static_cast<uint64_t>(sequence) + 1;
        auto& seen = seen_sequences_[static_cast<uint64_t>(sequence) % kDedupWindowSize];
        // 跨批次以首次接受的状态为准，不重复输出后续的丢失/收到状态修正。
        if (seen == tag)
            continue;

        seen = tag;
        newest_sequence_ = std::max(newest_sequence_, sequence);
        packets[unique_count++] = packet;
    }
    packets.resize(unique_count);
    return packets;
}

void PacketGroupObserver::Reset()
{
    seen_sequences_.fill(0);
    newest_sequence_ = -1;
    pending_group_.reset();
    previous_group_.reset();
    trend_history_.clear();
}
}
