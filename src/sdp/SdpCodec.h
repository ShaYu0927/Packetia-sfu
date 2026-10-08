#ifndef PACKETIA_SDP_CODEC_H_
#define PACKETIA_SDP_CODEC_H_

#include "SdpMode.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace sdp
{

struct H264CodecConfig
{
    uint32_t profileLevelId = 0x42e01f;
    int packetizationMode = 1;
    bool levelAsymmetryAllowed = true;
    // Raw NAL units, including their NAL header and without Annex-B prefixes.
    // Supply both or neither. An SPS overrides profileLevelId using its header.
    // Reserved profile bits and known level IDs are checked; full SPS/PPS
    // bitstream validation remains the codec's responsibility.
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};

struct H265CodecConfig
{
    int profileSpace = 0;
    int profileId = 1;
    bool tierFlag = false; // High tier requires Level 4 or higher (levelId >= 120).
    int levelId = 93;
    // Raw NAL units without Annex-B prefixes. Profile values remain explicit;
    // the builder checks NAL headers but does not parse HEVC parameter sets.
    std::vector<uint8_t> vps;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};

struct OpusCodecConfig
{
    std::optional<int> maxAverageBitrate;
    std::optional<int> maxPlaybackRate;
    std::optional<int> spropMaxCaptureRate;
    std::optional<int> minPtime;
    std::optional<bool> stereo;
    std::optional<bool> spropStereo;
    std::optional<bool> useInbandFec;
    std::optional<bool> useDtx;
    std::optional<bool> cbr;
};

struct AacCodecConfig
{
    // AAC-LC only, using an indexed MPEG-4 sampling frequency. Channel counts
    // 1..6 and 8 map to ASC channelConfiguration 1..7; PCE layouts are omitted.
    // AudioSpecificConfig and the AAC Audio Profile level follow rate/layout.
    // AAC-hbr fixes sizeLength/indexLength/indexDeltaLength to 13/3/3.
    int sampleRate = 48000;
    int channels = 2;
};

struct AmrWbCodecConfig
{
    int channels = 1; // RFC 4867 channel layouts: 1..6.
    bool octetAlign = true;
    std::vector<int> modeSet;
    std::optional<bool> crc;
    std::optional<bool> robustSorting;
    // Maximum frame-blocks in an interleaving group; positive when present.
    std::optional<int> interleaving;
};

// Codec descriptions and matching, independent of encoder/decoder and RTP I/O.
// Builders do not imply media support. The caller allocates PTs and may use
// Sdp::SetCodecs to apply descriptions to a media section in either profile.
// Invalid builder parameters throw std::invalid_argument; PTs are 0..127.
class SdpCodec
{
public:
    // Offer/answer codec matching, independent of media I/O. The offered PT is
    // preserved; RTCP feedback is negotiated by SdpNegotiator. Failure leaves
    // negotiated unchanged. Repair codecs require a separate RTP implementation.
    static bool Negotiate(const RtpCodecParameters& remote, const RtpCodecParameters& local,
                          RtpCodecParameters& negotiated);
    // Checks an answer against an offered codec, including format identity and
    // H264 level/asymmetry. Opus receive preferences may legitimately differ.
    static bool IsAnswer(const RtpCodecParameters& offer, const RtpCodecParameters& answer);
    // Compare RTP format identity independent of PT, fmtp ordering and feedback.
    static bool SameFormat(const RtpCodecParameters& a, const RtpCodecParameters& b);
    static RtpCodecParameters H264(int payloadType, const H264CodecConfig& config = {});
    static RtpCodecParameters H265(int payloadType, const H265CodecConfig& config = {});
    static RtpCodecParameters Opus(int payloadType, const OpusCodecConfig& config = {});
    static RtpCodecParameters Aac(int payloadType, const AacCodecConfig& config = {});
    static RtpCodecParameters AmrWb(int payloadType, const AmrWbCodecConfig& config = {});
};

} // namespace sdp

#endif // PACKETIA_SDP_CODEC_H_
