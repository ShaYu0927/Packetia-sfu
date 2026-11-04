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

    WorkJob job{};
    job.target_id = endpoint_id_;
    job.media_track_id = track_id_;
    if (packet.type == MediaPacketType::Rtcp)
    {
        job.type = WorkType::Rtcp;
        job.key = media_affinity::ResolveRtcpKey(endpoint_id_, packet.Data(), packet.Size());
    }
    else
    {
        job.type = WorkType::Rtp;
        job.key = media_affinity::ResolveRtpKey(endpoint_id_, packet.Data(), packet.Size());
    }
    job.enqueue_ts = packet.receive_time_ms;
    job.media_trace = trace;
    job.payload = std::move(packet.payload);

    return WorkerService::post(POOL_MEDIA, std::move(job)) == 0
        ? MediaPacketIngressResult::Accepted
        : MediaPacketIngressResult::Dropped;
}

} // namespace media::transport
