#include "TransportFeedbackGenerator.h"

#include <algorithm>

namespace rtcpx
{

TransportFeedbackGenerator::TransportFeedbackGenerator()
    : TransportFeedbackGenerator(Config{})
{
}

TransportFeedbackGenerator::TransportFeedbackGenerator(const Config& config)
    : config_(config)
{
    config_.feedback_interval_ms = std::max<uint32_t>(1, config_.feedback_interval_ms);
    config_.max_packet_status_count = std::max<size_t>(1, config_.max_packet_status_count);
    config_.max_history_packets = std::max(config_.max_packet_status_count, config_.max_history_packets);
}

int64_t TransportFeedbackGenerator::Unwrap(uint16_t sequence) const
{
    if (!has_sequence_)
    {
        return sequence;
    }

    int64_t candidate = (highest_extended_sequence_ & ~0xFFFFLL) | sequence;
    if (candidate + 0x8000 < highest_extended_sequence_)
    {
        candidate += 0x10000;
    }
    else if (candidate > highest_extended_sequence_ + 0x8000)
    {
        candidate -= 0x10000;
    }
    return candidate;
}

bool TransportFeedbackGenerator::OnPacket(uint16_t transport_sequence, int64_t arrival_time_us)
{
    if (arrival_time_us < 0)
    {
        return false;
    }

    const int64_t extended = Unwrap(transport_sequence);
    if (!has_sequence_)
    {
        has_sequence_ = true;
        highest_extended_sequence_ = extended;
        next_feedback_sequence_ = extended;
    }
    else if (extended < next_feedback_sequence_ && last_feedback_time_us_ != 0)
    {
        // 已经反馈过的迟到包不能回头修改历史反馈。
        return false;
    }
    else
    {
        highest_extended_sequence_ = std::max(highest_extended_sequence_, extended);
        // 第一批反馈生成前允许较早的乱序包向前扩展反馈起点。
        if (last_feedback_time_us_ == 0)
        {
            next_feedback_sequence_ = std::min(next_feedback_sequence_, extended);
        }
    }

    // 重复包保留第一次到达时间，因为它最能反映网络传输时延。
    arrivals_us_.emplace(extended, arrival_time_us);
    while (arrivals_us_.size() > config_.max_history_packets)
    {
        const int64_t discarded = arrivals_us_.begin()->first;
        arrivals_us_.erase(arrivals_us_.begin());
        // 过载时宁可显式跳过无法保存的历史，也不能把已收到但被淘汰的包
        // 在反馈中伪装成网络丢包。
        next_feedback_sequence_ = std::max(next_feedback_sequence_, discarded + 1);
    }
    return true;
}

bool TransportFeedbackGenerator::ShouldSendFeedback(int64_t now_us) const
{
    if (!has_sequence_ || highest_extended_sequence_ < next_feedback_sequence_)
    {
        return false;
    }
    if (static_cast<size_t>(highest_extended_sequence_ - next_feedback_sequence_ + 1) >=
        config_.max_packet_status_count)
    {
        return true;
    }
    return last_feedback_time_us_ == 0 ||
           now_us - last_feedback_time_us_ >=
               static_cast<int64_t>(config_.feedback_interval_ms) * 1000;
}

bool TransportFeedbackGenerator::BuildFeedback(uint32_t sender_ssrc,
                                                uint32_t media_ssrc,
                                                int64_t now_us,
                                                TransportFeedbackReport* report)
{
    if (!report || !ShouldSendFeedback(now_us))
    {
        return false;
    }

    const int64_t base = next_feedback_sequence_;
    const int64_t end = std::min(
        highest_extended_sequence_,
        base + static_cast<int64_t>(config_.max_packet_status_count) - 1);

    *report = TransportFeedbackReport{};
    report->sender_ssrc = sender_ssrc;
    report->media_ssrc = media_ssrc;
    report->base_sequence = static_cast<uint16_t>(base & 0xFFFF);
    report->packet_status_count = static_cast<uint16_t>(end - base + 1);
    report->feedback_packet_count = feedback_packet_count_++;
    report->packets.reserve(report->packet_status_count);

    int64_t first_arrival_us = -1;
    for (int64_t extended = base; extended <= end; ++extended)
    {
        TransportFeedbackPacket packet;
        packet.transport_sequence = static_cast<uint16_t>(extended & 0xFFFF);
        const auto arrival = arrivals_us_.find(extended);
        if (arrival != arrivals_us_.end())
        {
            packet.received = true;
            packet.receive_time_us = arrival->second;
            packet.receive_time_ms = static_cast<uint64_t>(arrival->second / 1000);
            if (first_arrival_us < 0)
            {
                first_arrival_us = arrival->second;
            }
        }
        report->packets.push_back(packet);
    }

    if (first_arrival_us >= 0)
    {
        report->reference_time_ms = static_cast<uint64_t>(first_arrival_us / 64000) * 64ULL;
    }

    arrivals_us_.erase(arrivals_us_.begin(), arrivals_us_.upper_bound(end));
    next_feedback_sequence_ = end + 1;
    last_feedback_time_us_ = now_us;
    return true;
}

void TransportFeedbackGenerator::Reset()
{
    arrivals_us_.clear();
    has_sequence_ = false;
    highest_extended_sequence_ = 0;
    next_feedback_sequence_ = 0;
    last_feedback_time_us_ = 0;
    feedback_packet_count_ = 0;
}

}
