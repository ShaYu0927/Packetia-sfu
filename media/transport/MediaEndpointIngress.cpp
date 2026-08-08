#include "MediaEndpointIngress.h"

#include "MediaStreamAffinity.h"
#include "ShardedWorkerPool.h"

#include <limits>
#include <memory>
#include <utility>

namespace media::transport
{

MediaPacketIngressResult MediaEndpointIngress::OnMediaPacket(ReceivedMediaPacket packet)
{
    if (!packet.IsValid() || endpoint_id_ == 0 || packet.Size() > std::numeric_limits<uint32_t>::max())
    {
        return MediaPacketIngressResult::Dropped;
    }

    auto owner = std::make_shared<ReceivedMediaPacket>(std::move(packet));

    uint32_t media_ssrc = 0;
    const bool has_media_ssrc = owner->type == MediaPacketType::Rtcp
        ? media_affinity::TryGetRtcpMediaSsrc(
              owner->Data(), owner->Size(), media_ssrc)
        : media_affinity::TryGetRtpSsrc(
              owner->Data(), owner->Size(), media_ssrc);

    WorkJob job{};
    job.target_id = endpoint_id_;
    job.key = has_media_ssrc
        ? media_affinity::MakeStreamHandle(endpoint_id_, media_ssrc).affinity_key
        : endpoint_id_;
    job.type = owner->type == MediaPacketType::Rtcp
        ? WorkType::Rtcp
        : WorkType::Rtp;
    job.raw.data = owner->Data();
    job.raw.len = static_cast<uint32_t>(owner->Size());
    job.enqueue_ts = owner->receive_time_ms;
    job.owner = std::move(owner);

    return WorkerService::post("media", std::move(job)) == 0 ? MediaPacketIngressResult::Accepted : MediaPacketIngressResult::Dropped;
}

} // namespace media::transport
