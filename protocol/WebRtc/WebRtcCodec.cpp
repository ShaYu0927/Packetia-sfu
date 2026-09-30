#include "WebRtcCodec.h"
#include "SdpCodec.h"

namespace protocol::webrtc
{
bool NegotiateRtpCodec(const RtpCodecParameters& remote,
                       const RtpCodecParameters& local,
                       RtpCodecParameters& negotiated)
{
    return sdp::SdpCodec::Negotiate(remote, local, negotiated);
}
} // namespace protocol::webrtc
