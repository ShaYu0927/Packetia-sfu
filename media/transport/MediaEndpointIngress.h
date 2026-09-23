#ifndef _MEDIA_ENDPOINT_INGRESS_H_
#define _MEDIA_ENDPOINT_INGRESS_H_

#include "IMediaPacketSink.h"

#include <cstdint>
#include <string>
#include <utility>

namespace media::transport
{

class MediaEndpointIngress final : public IMediaPacketSink
{
public:
    explicit MediaEndpointIngress(uint64_t endpoint_id, std::string track_id = {})
        : endpoint_id_(endpoint_id), track_id_(std::move(track_id))
    {
    }

    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override;

    uint64_t EndpointId() const noexcept { return endpoint_id_; }

private:
    const uint64_t endpoint_id_;
    const std::string track_id_;
};

} // namespace media::transport

#endif /* _MEDIA_ENDPOINT_INGRESS_H_ */
