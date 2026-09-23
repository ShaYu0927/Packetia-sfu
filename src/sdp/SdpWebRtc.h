#ifndef PACKETIA_SDP_WEBRTC_INTERNAL_H_
#define PACKETIA_SDP_WEBRTC_INTERNAL_H_

#include "SdpMode.h"

// Internal WebRTC profile implementation. Callers use Sdp::Parse/Serialize.
namespace sdp::detail
{
bool ParseWebRtcAttributes(const std::string& text, SdpSession& session, std::string& error);
SdpSession BuildWebRtcAttributes(const SdpSession& session);
}

#endif
