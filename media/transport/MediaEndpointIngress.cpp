#include "MediaEndpointIngress.h"

#include "MediaStreamAffinity.h"
#include "ShardedWorkerPool.h"

#include <utility>

namespace media::transport
{

MediaPacketIngressResult MediaEndpointIngress::OnMediaPacket(ReceivedMediaPacket packet)
{
    if (!packet.IsValid() || endpoint_id_ == 0)
    {
        return MediaPacketIngressResult::Dropped;
    }
    const auto trace = packet.type == MediaPacketType::Rtp
        ? media_latency::BeginPacket() : media_latency::PacketTrace{};

    uint32_t media_ssrc = 0;
    const bool has_media_ssrc = packet.type == MediaPacketType::Rtcp
        ? media_affinity::TryGetRtcpMediaSsrc(
              packet.Data(), packet.Size(), media_ssrc)
        : media_affinity::TryGetRtpSsrc(
              packet.Data(), packet.Size(), media_ssrc);

    WorkJob job{};
    job.target_id = endpoint_id_;
    job.key = has_media_ssrc ? media_affinity::MakeStreamHandle(endpoint_id_, media_ssrc).affinity_key : endpoint_id_;
    job.type = packet.type == MediaPacketType::Rtcp ? WorkType::Rtcp : WorkType::Rtp;
    job.enqueue_ts = packet.receive_time_ms;
    job.media_trace = trace;
    job.payload = std::move(packet.payload);

    return WorkerService::post(POOL_MEDIA, std::move(job)) == 0
        ? MediaPacketIngressResult::Accepted
        : MediaPacketIngressResult::Dropped;
}

} // namespace media::transport
