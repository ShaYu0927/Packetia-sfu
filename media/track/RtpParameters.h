#ifndef PACKETIA_MEDIA_RTP_PARAMETERS_H
#define PACKETIA_MEDIA_RTP_PARAMETERS_H

#include "SdpMode.h"
#include <optional>

namespace media
{
enum class RtpFlow { Send, Receive };

struct RtpEncodingParameters
{
    uint32_t ssrc = 0;
    std::optional<uint32_t> rtx_ssrc;
    std::string cname;
};

// Parameters of ONE negotiated RTP session, separate from MediaTrack's business
// metadata. Publisher and subscriber have different instances/PT namespaces.
// The SDP RTP value types are reused; no SDP session or transport is retained.
struct RtpParameters
{
    std::string kind;
    std::string mid;
    // Directions are relative to this endpoint (use the local negotiated SDP).
    sdp::MediaDirection direction = sdp::MediaDirection::SendRecv;
    sdp::RtpCodecParameters codec;
    // RTX's fmtp retains apt and optional rtx-time. Sender bindings also need
    // an RTX SSRC; incoming repair packets are restored before primary input.
    std::optional<sdp::RtpCodecParameters> rtx;
    std::vector<sdp::RtpHeaderExtensionParameters> header_extensions;
    std::vector<RtpEncodingParameters> encodings;
    bool rtcp_mux = false;
    bool rtcp_reduced_size = false;
    std::string control; // Legacy RTSP control URI; not business metadata.
    bool feedback_negotiated = false; // Generic RTSP retains its legacy policy.

    bool HasFeedback(const std::string& type, const std::string& parameter = {}) const
    {
        for (const auto& feedback : codec.rtcpFeedback)
            if (feedback.type == type && feedback.parameter == parameter) return true;
        return false;
    }

    bool Allows(RtpFlow flow) const
    {
        return direction == sdp::MediaDirection::SendRecv ||
            direction == (flow == RtpFlow::Send ? sdp::MediaDirection::SendOnly : sdp::MediaDirection::RecvOnly);
    }

    uint8_t ExtensionId(const std::string& uri, RtpFlow flow) const
    {
        if (!Allows(flow)) return 0;
        for (const auto& ext : header_extensions)
            if (ext.id > 0 && ext.id <= 255 && ext.uri == uri && (ext.direction == sdp::MediaDirection::SendRecv ||
                ext.direction == (flow == RtpFlow::Send ? sdp::MediaDirection::SendOnly : sdp::MediaDirection::RecvOnly)))
                return static_cast<uint8_t>(ext.id);
        return 0;
    }
};
}
#endif
