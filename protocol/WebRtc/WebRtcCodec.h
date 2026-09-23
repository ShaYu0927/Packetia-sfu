#ifndef PACKETIA_WEBRTC_CODEC_H
#define PACKETIA_WEBRTC_CODEC_H

#include "WebRtc_Config.h"

namespace protocol::webrtc
{

// Build one answer codec using the offered payload type. Failure leaves
// negotiated unchanged; RTCP feedback is left for the session to negotiate.
//
// H.264 supports the common Baseline/Main/Extended/High profiles and their
// constrained variants, packetization modes 0/1, and level asymmetry. Additional
// H.264 limits and out-of-band parameter sets are deliberately unsupported.
// Missing profile-level-id uses WebRTC's deployed 42e01f convention, rather
// than RFC 6184's Baseline Level 1 default. This negotiates signaling only;
// the caller must configure/enforce the corresponding encoder/decoder limits.
//
// Opus answers advertise local receive preferences and local sprop sender
// hints. Remote receive preferences remain in the offer for the local sender;
// they must not be inferred from this answer codec. Unknown Opus fmtp keys are
// ignored as required by RFC 7587. The deployed integer minptime extension is
// accepted for values from 3 through 120 milliseconds.
//
// Other explicitly configured codecs require matching fmtp parameter maps.
// RTX, RED and FEC formats require dependent-payload negotiation and are not
// supported here. This conservative fallback does not negotiate their limits.
bool NegotiateRtpCodec(const RtpCodecParameters& remote,
                       const RtpCodecParameters& local,
                       RtpCodecParameters& negotiated);

} // namespace protocol::webrtc

#endif // PACKETIA_WEBRTC_CODEC_H
