#ifndef PACKETIA_MEDIA_ENDPOINT_RECEIVE_FEEDBACK_HANDLER_H_
#define PACKETIA_MEDIA_ENDPOINT_RECEIVE_FEEDBACK_HANDLER_H_

#include "ReceiveQualityMonitor.h"
#include "RtpReceiver.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace media::endpoint
{
// 接收侧反馈协调：推进 NACK、采集统计、发送 RR，并调用质量模块。
// 返回是否需要请求视频关键帧；调用方持有端点状态锁，负责接收器和发送出口的生命周期。
class ReceiveFeedbackHandler
{
public:
    using SendCallback = std::function<bool(const uint8_t*, std::size_t)>;

    bool RegisterSource(uint32_t source_ssrc);
    std::optional<NetworkStatsSnapshot> GetSnapshot(uint32_t source_ssrc, uint64_t now_ms = 0) const;
    bool Evaluate(const rtsp::RtpReceiverTrack::Ptr& track, uint64_t now_ms,
                  uint32_t local_rtcp_ssrc, const SendCallback& send);
    void Remove(uint32_t ssrc);
    void Clear();

private:
    ReceiveQualityMonitor quality_;
};
}

#endif // PACKETIA_MEDIA_ENDPOINT_RECEIVE_FEEDBACK_HANDLER_H_
