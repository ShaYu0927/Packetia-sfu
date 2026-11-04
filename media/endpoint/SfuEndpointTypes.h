#ifndef PACKETIA_MEDIA_ENDPOINT_SFU_ENDPOINT_TYPES_H_
#define PACKETIA_MEDIA_ENDPOINT_SFU_ENDPOINT_TYPES_H_

#include "IMediaTransport.h"
#include "KeyFrameRequestLimiter.h"
#include "MediaTrackSubscription.h"
#include "Rtp.h"
#include "RtpSenderTrack.h"
#include "RtpTypes.h"
#include "DownlinkBandwidthAllocator.h"
#include "VideoResumeBuffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media
{
class SfuEndpoint;

// 端点内部运行时类型，读写仍由 SfuEndpoint 的执行器和锁约束。
namespace endpoint
{
using SendRtcpCallback = std::function<bool(const uint8_t*, std::size_t)>;

struct Subscription;

// 发布源的协商描述、下游关系与共享关键帧请求预算。
struct PublishedTrack
{
    std::shared_ptr<RtpTrackDescription> description;       /* RTP track description shared by the published track. */
    std::string mid;                                        /* Media identifier associated with this track. */
    uint8_t mid_extension_id = 0;                           /* RTP MID header extension ID; 0 means unset. */
    SendRtcpCallback send_rtcp;                             /* Callback for sending RTCP feedback to the publisher. */
    std::vector<std::weak_ptr<Subscription>> subscribers;   /* Subscribers referenced without extending their lifetimes. */
    KeyFrameRequestLimiter keyframe_requests;               /* Rate limiter for keyframe requests to the publisher. */
    std::shared_ptr<SourceBitrateEstimator> source_bitrate = std::make_shared<SourceBitrateEstimator>(); /* Shared estimator for the source track bitrate. */
};

enum class DownlinkBandwidthState { Forwarding, Paused, WaitingKeyFrame };

struct DownlinkBandwidthStatus
{
    DownlinkBandwidthState state = DownlinkBandwidthState::Forwarding;
    DownlinkSubscriptionPolicy policy;
    uint32_t required_bps = 0;
};

// 一条端点转发关系，弱引用两端；拥有本订阅的发送器和投递闸门。
struct Subscription
{
    std::atomic<bool> active{true};
    std::weak_ptr<SfuEndpoint> source;
    std::weak_ptr<SfuEndpoint> destination;
    std::string source_track_id;
    // 选中的编码源，由发布端点同步访问。
    std::optional<uint32_t> source_ssrc;
    std::shared_ptr<rtsp::RtpSenderTrack> sender;
    MediaTrackSubscription::Ptr consumer;
    std::shared_ptr<IMediaTransport> send_transport;
    std::shared_ptr<SourceBitrateEstimator> source_bitrate;
    DownlinkSubscriptionPolicy bandwidth_policy;
    std::atomic<DownlinkBandwidthState> bandwidth_state{DownlinkBandwidthState::Forwarding};
    std::atomic<bool> probe_allowed{false};
    uint64_t transport_id = 0;
    CodecId codec = CodecId::Unknown;
    bool audio = false;
    rtsp::VideoResumeBuffer recovery;
    uint64_t next_keyframe_request_ms = 0;

    bool Closed() const
    {
        return !active.load() || (consumer && consumer->Closed());
    }

    bool CanDeliver() const
    {
        return active.load() && (!consumer || consumer->CanDeliver());
    }

    bool CanSendMedia() const
    {
        return CanDeliver() && bandwidth_state.load() == DownlinkBandwidthState::Forwarding;
    }

    void Close()
    {
        active.store(false);
        if (consumer)
        {
            consumer->Close();
        }
    }
};

// 订阅者连接协商出的下行参数，在创建转发关系前配置。
struct SubscriptionParameters
{
    rtsp::RtpSenderTrackConfig config;
    std::shared_ptr<IMediaTransport> transport;
    CodecId codec = CodecId::Unknown;
};

// 暂停期间保留轨道身份和 SSRC，供后续协商恢复。
struct PausedPublishedTrack
{
    PublishedTrack track;
    std::vector<uint32_t> ssrcs;
};

}
}

#endif // PACKETIA_MEDIA_ENDPOINT_SFU_ENDPOINT_TYPES_H_
