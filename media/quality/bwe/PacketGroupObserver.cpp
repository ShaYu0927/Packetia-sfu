#include "PacketGroupObserver.h"

#include <algorithm>

namespace media
{
PacketGroupObservation PacketGroupObserver::OnFeedback(const TransportFeedback& feedback,
                                                       const PacketGroupContext&)
{
    PacketGroupObservation result;

    const auto packets = SortAndDeduplicate(feedback);
    for (const auto* packet : packets)
    {
        if (auto completed = AddPacketToGroup(*packet))
        {
            completed->feedback_time_ms = feedback.feedback_time_ms;
            ObserveCompletedGroup(*completed, result);
            result.completed_groups.push_back(*completed);
        }
    }

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
    ResetTrend();
}

void PacketGroupObserver::ResetTrend()
{
    previous_group_.reset();
    trend_estimator_.Reset();
}

std::optional<PacketGroup> PacketGroupObserver::AddPacketToGroup(const PacketFeedback& packet)
{
    const uint64_t send_time_ms = packet.sent_packet.send_time_ms;
    // 已完成组不再修改；先检查倒退时间，避免无符号减法下溢。
    if (pending_group_ && send_time_ms < pending_group_->first_send_time_ms)
        return std::nullopt;

    std::optional<PacketGroup> completed;
    if (pending_group_ && send_time_ms - pending_group_->first_send_time_ms > kGroupDurationMs)
    {
        completed = *pending_group_;
        pending_group_.reset();
    }

    if (!pending_group_)
    {
        pending_group_.emplace();
        pending_group_->id = completed ? completed->id + 1 : 0;
        pending_group_->first_send_time_ms = send_time_ms;
    }

    auto& group = *pending_group_;
    group.last_send_time_ms = std::max(group.last_send_time_ms, send_time_ms);
    if (packet.received)
    {
        ++group.received_packets;
        group.received_bytes += packet.sent_packet.size_bytes;
        if (!group.last_received_send_time_ms || send_time_ms > *group.last_received_send_time_ms)
            group.last_received_send_time_ms = send_time_ms;
        if (!group.first_receive_time_ms || packet.receive_time_ms < *group.first_receive_time_ms)
            group.first_receive_time_ms = packet.receive_time_ms;
        if (!group.last_receive_time_ms || packet.receive_time_ms > *group.last_receive_time_ms)
            group.last_receive_time_ms = packet.receive_time_ms;
    }
    else
    {
        ++group.lost_packets;
        group.lost_bytes += packet.sent_packet.size_bytes;
    }

    return completed;
}

// Calculate group deltas, then delegate smoothing, regression and detection
// to the vendored WebRTC trendline estimator. See docs/webrtc-trendline.md.
void PacketGroupObserver::ObserveCompletedGroup(const PacketGroup& group, PacketGroupObservation& result)
{
    // 1. 没有成功接收的包或有效时间，就无法计算组间延迟变化。
    if (!group.received_packets || !group.last_received_send_time_ms || !group.last_receive_time_ms)
        return;

    // The first valid group establishes a baseline for subsequent group deltas.
    if (!previous_group_)
    {
        previous_group_ = group;
        return;
    }

    // 2. 使用各组已接收包的最晚发送时间、最晚接收时间作为组的代表时间。
    // 两个最晚时间不一定属于同一个包，因此这里比较的是组级间隔。
    const uint64_t send_ms = *group.last_received_send_time_ms;
    const uint64_t receive_ms = *group.last_receive_time_ms;
    const uint64_t previous_send_ms = *previous_group_->last_received_send_time_ms;
    const uint64_t previous_receive_ms = *previous_group_->last_receive_time_ms;
    // 接收乱序不能推进参考时间，也不能进行无符号倒退减法。
    if (send_ms <= previous_send_ms || receive_ms <= previous_receive_ms)
        return;

    const uint64_t send_delta_ms = send_ms - previous_send_ms;
    const uint64_t receive_delta_ms = receive_ms - previous_receive_ms;
    // 间隔过大视为观测中断，旧的累计值、窗口和检测状态不能继续沿用。
    if (send_delta_ms > kMaxTrendGapMs || receive_delta_ms > kMaxTrendGapMs)
    {
        ResetTrend();
        previous_group_ = group;
        // 同一批反馈前面的旧窗口结果也不能跨越中断继续使用。
        result.usage.reset();
        result.delay_trend_ms.reset();
        result.delay_trend_slope.reset();
        return;
    }
    previous_group_ = group;

    // Pass raw group intervals exactly once. The estimator owns accumulation,
    // EWMA smoothing, the regression window and adaptive overuse detection.
    const auto observation = trend_estimator_.Update(receive_delta_ms, send_delta_ms, receive_ms);
    if (!observation) return;
    result.usage = observation->usage;
    result.delay_trend_ms = observation->smoothed_delay_ms;
    result.delay_trend_slope = observation->slope;
}

}
