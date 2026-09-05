#ifndef PACKETIA_RTCP_TRANSPORT_FEEDBACK_GENERATOR_H_
#define PACKETIA_RTCP_TRANSPORT_FEEDBACK_GENERATOR_H_

#include "RtcpContext.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace rtcpx
{

/**
 * 接收端 TWCC 到达记录器和反馈报告生成器。
 *
 * 一个实例对应一条接收 Transport，而不是一个 SSRC。音频和视频 RTP
 * 使用同一 transport sequence 空间，因此必须共同写入同一个生成器。
 * 本类只生成标准化报告，不负责 RTCP 字节序列化和网络发送。
 * 所有方法应在所属 Transport 的同一 IO/媒体线程调用。
 */
class TransportFeedbackGenerator
{
public:
    struct Config
    {
        uint32_t feedback_interval_ms = 100;
        size_t max_packet_status_count = 512;
        size_t max_history_packets = 4096;
    };

    TransportFeedbackGenerator();
    explicit TransportFeedbackGenerator(const Config& config);

    // arrival_time_us 必须使用本地单调时钟，单位为微秒。
    bool OnPacket(uint16_t transport_sequence, int64_t arrival_time_us);

    bool ShouldSendFeedback(int64_t now_us) const;

    // 构造下一批反馈；序号间隙会自动生成 received=false 的状态项。
    bool BuildFeedback(uint32_t sender_ssrc,
                       uint32_t media_ssrc,
                       int64_t now_us,
                       TransportFeedbackReport* report);

    void Reset();
    size_t PendingPacketCount() const { return arrivals_us_.size(); }

private:
    int64_t Unwrap(uint16_t sequence) const;

    Config config_;
    std::map<int64_t, int64_t> arrivals_us_;
    bool has_sequence_ = false;
    int64_t highest_extended_sequence_ = 0;
    int64_t next_feedback_sequence_ = 0;
    int64_t last_feedback_time_us_ = 0;
    uint8_t feedback_packet_count_ = 0;
};

}

#endif /* PACKETIA_RTCP_TRANSPORT_FEEDBACK_GENERATOR_H_ */
