#ifndef PACKETIA_MEDIA_RTP_SENDER_BINDING_H
#define PACKETIA_MEDIA_RTP_SENDER_BINDING_H

#include "SdpTrackBinding.h"
#include "RtpSenderTrack.h"

namespace media
{
// Use subscriber parameters; preserve caller-supplied cache/recovery policy.
// A sender forwards one selected primary encoding and its optional RTX stream.
inline bool BindRtpSenderParameters(const RtpParameters& params, rtsp::RtpSenderTrackConfig& config)
{
    if (!params.Allows(RtpFlow::Send) || params.encodings.size() != 1 ||
        !params.encodings.front().ssrc ||
        !IsSupportedRtpCodec(params.kind, params.codec) || params.codec.payloadType < 0 ||
        params.codec.payloadType > 127 || params.codec.clockRate <= 0) return false;
    auto bound = config;
    const auto& encoding = params.encodings.front();
    if (params.rtx.has_value() != encoding.rtx_ssrc.has_value()) return false;
    bound.rtx_payload_type.reset();
    bound.rtx_ssrc = 0;
    if (params.rtx) {
        if (!sdp::SdpCodec::IsRepairCodec(*params.rtx) ||
            sdp::SdpCodec::AssociatedPayloadType(*params.rtx) != params.codec.payloadType ||
            params.rtx->payloadType < 0 || params.rtx->payloadType > 127 ||
            params.rtx->payloadType == params.codec.payloadType ||
            params.rtx->clockRate != params.codec.clockRate ||
            std::max(1, params.rtx->channels) != std::max(1, params.codec.channels) ||
            !*encoding.rtx_ssrc || *encoding.rtx_ssrc == encoding.ssrc) return false;
        bound.rtx_payload_type = static_cast<uint8_t>(params.rtx->payloadType);
        bound.rtx_ssrc = *encoding.rtx_ssrc;
    }
    bound.nack_enabled = !params.feedback_negotiated || params.HasFeedback("nack");
    bound.pli_enabled = !params.feedback_negotiated || params.HasFeedback("nack", "pli");
    bound.fir_enabled = !params.feedback_negotiated || params.HasFeedback("ccm", "fir");
    if (!bound.nack_enabled) bound.rtp_cache_size = 0;
    bound.local_ssrc = params.encodings.front().ssrc;
    bound.payload_type = static_cast<uint8_t>(params.codec.payloadType);
    bound.sample_rate = params.codec.clockRate;
    bound.rewrite_payload_type = true;
    bound.rewrite_header_extensions = true;
    bound.mid_extension_id = params.ExtensionId("urn:ietf:params:rtp-hdrext:sdes:mid", RtpFlow::Send);
    bound.transport_cc_extension_id = params.ExtensionId(
        "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", RtpFlow::Send);
    bound.mid = bound.mid_extension_id ? params.mid : std::string{};
    if (bound.mid_extension_id > 14 || bound.transport_cc_extension_id > 14 ||
        (bound.mid_extension_id && (bound.mid.empty() || bound.mid.size() > 16 ||
            bound.mid_extension_id == bound.transport_cc_extension_id))) return false;
    config = std::move(bound);
    return true;
}
}
#endif
