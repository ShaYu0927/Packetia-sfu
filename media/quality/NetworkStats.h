#ifndef _NET_WORK_STATS_H_
#define _NET_WORK_STATS_H_

#include <cstdint>
#include <vector>
namespace media 
{
enum class MediaKind
{
    Unknown = 0,
    Audio,
    Video,
    Retransmission,
    Padding,
};


enum class NetworkQualityLevel
{
    Unknown = 0,
    Excellent,
    Good,
    Weak,
    Bad,
};


inline const char* ToString(NetworkQualityLevel level)
{
    switch (level)
    {
    case NetworkQualityLevel::Excellent:
        return "Excellent";
    case NetworkQualityLevel::Good:
        return "Good";
    case NetworkQualityLevel::Weak:
        return "Weak";
    case NetworkQualityLevel::Bad:
        return "Bad";
    default:
        return "Unknown";
    }
}

struct BitrateConstraints
{
    uint32_t min_bitrate_bps = 100 * 1000;      /* 100 kbps*/
    uint32_t start_bitrate_bps = 800 * 1000;    /* 800 kbps */
    uint32_t max_bitrate_bps = 2500 * 1000;     /* 2500 kbps = 2.5 Mbps */
};


struct PacketSendInfo
{
    int64_t transport_sequence = -1;

    // RTP 层信息，方便定位是哪一路流
    uint32_t ssrc = 0;
    uint16_t rtp_sequence = 0;

    uint64_t send_time_ms = 0;
    uint32_t size_bytes = 0;

    MediaKind media_kind = MediaKind::Unknown;

    // 发送该包时，网络中还未确认的数据量
    uint32_t data_in_flight_bytes = 0;

    // 是否是探测包
    bool is_probe = false;
    int probe_cluster_id = -1;
};


struct PacketFeedback
{
    PacketSendInfo sent_packet;
    bool received = false;
    uint64_t receive_time_ms = 0;

    uint32_t DelayMs() const
    {
        if (!received)
        {
            return 0;
        }

        if (receive_time_ms < sent_packet.send_time_ms)
        {
            return 0;
        }

        return static_cast<uint32_t>(receive_time_ms - sent_packet.send_time_ms);
    }
};


struct TransportFeedback
{
    uint64_t feedback_time_ms = 0;
    uint32_t data_in_flight_bytes = 0;
    std::vector<PacketFeedback> packet_feedbacks;

    uint32_t PacketCount() const
    {
        return static_cast<uint32_t>(packet_feedbacks.size());
    }

    uint32_t ReceivedCount() const
    {
        uint32_t count = 0;

        for (const auto& item : packet_feedbacks)
        {
            if (item.received)
            {
                ++count;
            }
        }

        return count;
    }

    uint32_t LostCount() const
    {
        const uint32_t total = PacketCount();
        const uint32_t received = ReceivedCount();

        if (total < received)
        {
            return 0;
        }

        return total - received;
    }

    double LossRate() const
    {
        const uint32_t total = PacketCount();
        if (total == 0)
        {
            return 0.0;
        }

        return static_cast<double>(LostCount()) / static_cast<double>(total);
    }
};

struct LossReport
{
    uint64_t lost_packets = 0;
    uint64_t received_packets = 0;

    double LossRate() const
    {
        const uint64_t total = lost_packets + received_packets;
        if (total == 0)
        {
            return 0.0;
        }

        return static_cast<double>(lost_packets) / static_cast<double>(total);
    }
};

struct NetworkEstimate
{
    uint64_t update_time_ms = 0;

    uint32_t send_bitrate_bps = 0;
    uint32_t recv_bitrate_bps = 0;

    // 估算出来的可用带宽
    uint32_t bandwidth_bps = 0;

    uint32_t rtt_ms = 0;
    uint32_t jitter_ms = 0;

    // 0.05 表示 5% 丢包
    double loss_rate = 0.0;

    NetworkQualityLevel quality = NetworkQualityLevel::Unknown;
};

struct TargetTransferRate
{
    uint64_t update_time_ms = 0;

    NetworkEstimate estimate;

    // 当前建议发送码率
    uint32_t target_bitrate_bps = 0;

    // 更稳定的目标码率，防止码率频繁抖动
    uint32_t stable_target_bitrate_bps = 0;
};

struct PacerConfig
{
    uint64_t update_time_ms = 0;

    // Pacer 平滑发送码率
    uint32_t pacing_bitrate_bps = 0;

    // padding 探测码率
    uint32_t padding_bitrate_bps = 0;

    // 默认 40ms 一个发送窗口
    uint32_t time_window_ms = 40;
};

struct NetworkControlUpdate
{
    bool has_target_rate = false;
    TargetTransferRate target_rate;

    bool has_pacer_config = false;
    PacerConfig pacer_config;

    bool enable_nack = true;
    bool enable_fec = false;

    bool need_probe = false;
    uint32_t probe_bitrate_bps = 0;

    bool request_key_frame = false;

    bool HasUpdates() const
    {
        return has_target_rate || has_pacer_config || need_probe || request_key_frame;
    }
};

struct NetworkStatsSnapshot
{
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
    uint64_t packets_lost = 0;

    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;

    uint32_t send_bitrate_bps = 0;
    uint32_t recv_bitrate_bps = 0;
    uint32_t target_bitrate_bps = 0;

    uint32_t rtt_ms = 0;
    uint32_t jitter_ms = 0;
    double loss_rate = 0.0;

    uint64_t nack_count = 0;
    uint64_t pli_count = 0;
    uint64_t fir_count = 0;

    uint64_t update_time_ms = 0;

    uint32_t data_in_flight_bytes = 0;

    NetworkQualityLevel quality = NetworkQualityLevel::Unknown;
};

}

#endif /* _NET_WORK_STATS_H_ */