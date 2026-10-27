#ifndef PACKETIA_MEDIA_SDP_TRACK_BINDING_H_
#define PACKETIA_MEDIA_SDP_TRACK_BINDING_H_

#include "RtpStreamContext.h"
#include <memory>
#include <string>

namespace sdp
{
struct SdpMedia;
struct SdpSession;
}

namespace media
{
// SDP 适配入口。公共 RTP 值类型及选择逻辑不依赖 SDP 或旧接收器。
class RtpStreamContextBuilder
{
public:
    static RtpTrackContext BuildTrackFromSdp(const sdp::SdpMedia& media, int index);
    static std::shared_ptr<const RtpStreamContext> BuildFromSdp(const sdp::SdpSession& session);
};

uint8_t FindRtpExtensionId(const sdp::SdpMedia& media, const std::string& uri);
}

#endif
