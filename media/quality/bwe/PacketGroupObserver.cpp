#include "PacketGroupObserver.h"

#include <algorithm>
#include <cmath>

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
    trend_sample_count_ = next_trend_sample_ = 0;
    trend_origin_ms_ = 0;
    num_deltas_ = 0;
    accumulated_delay_ms_ = smoothed_delay_ms_ = previous_slope_ = 0;
    threshold_ = 12.5;
    overuse_duration_ms_ = 0;
    overuse_count_ = 0;
    trend_usage_ = BandwidthUsage::kNormal;
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

// 从相邻已完成组的发送/接收间隔中提取延迟变化，再根据窗口内的回归斜率判断拥塞。
// 数据流：组间延迟变化 → 累计 → EWMA 平滑 → 窗口回归 → 持续性判断 → 输出。
void PacketGroupObserver::ObserveCompletedGroup(const PacketGroup& group, PacketGroupObservation& result)
{
    // 1. 没有成功接收的包或有效时间，就无法计算组间延迟变化。
    if (!group.received_packets || !group.last_received_send_time_ms || !group.last_receive_time_ms)
        return;

    // 首个有效组只作为比较基准；同时记录回归横轴的时间原点。
    if (!previous_group_)
    {
        previous_group_ = group;
        trend_origin_ms_ = *group.last_receive_time_ms;
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
        trend_origin_ms_ = receive_ms;
        // 同一批反馈前面的旧窗口结果也不能跨越中断继续使用。
        result.usage.reset();
        result.delay_trend_ms.reset();
        result.delay_trend_slope.reset();
        return;
    }
    previous_group_ = group;

    // 3. 单次延迟变化 d = 接收间隔 ΔR - 发送间隔 ΔS，单位 ms。
    // 例如 ΔS=10、ΔR=15，则 d=+5，说明本组比上组多经历了约 5 ms 延迟。
    // 累计值反映相对起点的延迟变化，不是绝对单向时延或绝对排队时延。
    accumulated_delay_ms_ += static_cast<double>(receive_delta_ms) - static_cast<double>(send_delta_ms);
    // EWMA：保留 90% 旧值，加入 10% 新累计值，减轻单次抖动的影响。
    smoothed_delay_ms_ = 0.9 * smoothed_delay_ms_ + 0.1 * accumulated_delay_ms_;

    // 4. 环形窗口保存最近 kTrendWindowSize（当前为 20）个样本：
    // x = 相对接收时间，y = 平滑后的累计延迟变化，二者单位均为 ms。
    // 下标回绕后覆盖最老样本；回归只需各样本的坐标，不要求数组按时间排序。
    trend_history_[next_trend_sample_] = {
        static_cast<double>(receive_ms - trend_origin_ms_), smoothed_delay_ms_};
    next_trend_sample_ = (next_trend_sample_ + 1) % kTrendWindowSize;
    trend_sample_count_ = std::min(trend_sample_count_ + 1, kTrendWindowSize);
    // 有效间隔计数用于后面的趋势缩放，最多累计到 60，避免放大系数无限增长。
    num_deltas_ = std::min(num_deltas_ + 1, 60U);
    // 窗口未满时继续收集，不生成新的趋势判断。
    if (trend_sample_count_ < kTrendWindowSize)
        return;

    // 5. 最小二乘法拟合 y ≈ slope*x + intercept，先求 x、y 的平均值。
    double mean_time = 0;
    double mean_delay = 0;
    for (const auto& sample : trend_history_)
    {
        mean_time += sample.arrival_time_ms;
        mean_delay += sample.smoothed_delay_ms;
    }
    mean_time /= kTrendWindowSize;
    mean_delay /= kTrendWindowSize;
    // slope = Σ[(x-平均x)*(y-平均y)] / Σ[(x-平均x)^2]。
    // 分子、分母都省略相同的归一化因子，相除后的斜率不变。
    double covariance = 0;
    double variance = 0;
    for (const auto& sample : trend_history_)
    {
        const double time = sample.arrival_time_ms - mean_time;
        covariance += time * (sample.smoothed_delay_ms - mean_delay);
        variance += time * time;
    }
    // 横轴没有跨度时无法拟合，避免除零。
    if (variance <= 0)
        return;
    // 单位 ms/ms：正值表示延迟总体增加，负值表示下降，接近零表示平稳。
    const double slope = covariance / variance;
    // 6. 将原始斜率按有效间隔数和经验系数 4.0 缩放，再与检测阈值比较。
    // modified_trend 是检测量，不应当作为毫秒延迟展示。
    const double modified_trend = num_deltas_ * 4.0 * slope;
    if (modified_trend > threshold_)
    {
        // 首次越界计半个发送间隔，后续累计；持续时间和次数都设上限。
        overuse_duration_ms_ = overuse_count_ == 0 ? send_delta_ms * 0.5 :
            std::min(overuse_duration_ms_ + send_delta_ms, 2000.0);
        overuse_count_ = std::min(overuse_count_ + 1, 2U);
        // 需要越界超过 10 ms、至少连续两次，且斜率未下降，才确认过度使用。
        // 条件尚未满足时保留此前的检测状态，避免单次尖峰直接触发降码率。
        if (overuse_duration_ms_ > 10.0 && overuse_count_ > 1 && slope >= previous_slope_)
            trend_usage_ = BandwidthUsage::kOverUsing;
    }
    else
    {
        // 离开正向越界区后清除持续性计数；明显下降判为 UnderUsing，否则 Normal。
        overuse_duration_ms_ = 0;
        overuse_count_ = 0;
        trend_usage_ = modified_trend < -threshold_ ?
            BandwidthUsage::kUnderUsing : BandwidthUsage::kNormal;
    }
    previous_slope_ = slope;

    // 7. 阈值跟随一般幅度的检测量自适应，突发大尖峰不用于抬高阈值。
    // 阈值下降用较大增益，上升用较小增益；限制时间步长及阈值范围。
    const double magnitude = std::abs(modified_trend);
    if (magnitude <= threshold_ + 15.0)
    {
        const double gain = magnitude < threshold_ ? 0.039 : 0.0087;
        threshold_ = std::clamp(threshold_ + gain * (magnitude - threshold_) *
            std::min<uint64_t>(receive_delta_ms, 100), 6.0, 600.0);
    }
    // 8. 输出状态、平滑累计延迟变化和原始斜率，供控制器及诊断使用。
    result.usage = trend_usage_;
    result.delay_trend_ms = smoothed_delay_ms_;
    result.delay_trend_slope = slope;
}

}
