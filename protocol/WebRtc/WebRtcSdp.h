#ifndef PACKETIA_WEBRTC_SDP_H_
#define PACKETIA_WEBRTC_SDP_H_

#include "Sdp.h"
#include "WebRtc_Config.h"

namespace protocol::webrtc
{
// Compatibility entry points; parsing, validation and serialization live in
// src/sdp. New code should use Sdp::Parse with SdpProfile::WebRtc directly.
inline bool ParseWebRtcSdp(const std::string& text, SdpType type,
    WebRtcSessionDescription& output, std::string& error)
{
    return sdp::Sdp::Parse(text, sdp::SdpProfile::WebRtc, type, output, error);
}

inline std::string SerializeWebRtcSdp(const WebRtcSessionDescription& description)
{
    auto session = description;
    session.profile = sdp::SdpProfile::WebRtc;
    return sdp::Sdp::Serialize(session);
}
} // namespace protocol::webrtc

#endif // PACKETIA_WEBRTC_SDP_H_
