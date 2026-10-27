#ifndef PACKETIA_WEBRTC_CODEC_H
#define PACKETIA_WEBRTC_CODEC_H

#include "WebRtc_Config.h"

namespace protocol::webrtc
{

bool NegotiateRtpCodec(const RtpCodecParameters& remote, const RtpCodecParameters& local, RtpCodecParameters& negotiated);

} // namespace protocol::webrtc

#endif // PACKETIA_WEBRTC_CODEC_H
