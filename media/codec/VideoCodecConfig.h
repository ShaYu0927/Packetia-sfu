#pragma once

#include "MediaFrameTypes.h"

#include <string>
#include <variant>

namespace media::codec {

enum class CodecErrorCode { None, Unsupported, InvalidConfiguration, InitializationFailed, RuntimeFailed };
struct CodecError
{
    CodecErrorCode code = CodecErrorCode::None;
    std::string message;
};

enum class SelectionMode { Auto, Prefer, Require };
struct BackendRequest
{
    SelectionMode mode = SelectionMode::Auto;
    std::string implementation;
    bool Valid() const noexcept
    {
        return (mode == SelectionMode::Auto || mode == SelectionMode::Prefer || mode == SelectionMode::Require) &&
            (mode != SelectionMode::Require || !implementation.empty());
    }
};

enum class H264Profile { Baseline, Main, High };
struct H264Parameters { H264Profile profile = H264Profile::Baseline; };
struct H265Parameters {}; // Main, 8-bit 4:2:0 in this adapter.
struct Vp8Parameters {};
struct Vp9Parameters { int profile = 0; };
struct Av1Parameters { int profile = 0; };
// monostate selects the documented default for the chosen codec.
using VideoCodecParameters = std::variant<std::monostate, H264Parameters, H265Parameters,
                                         Vp8Parameters, Vp9Parameters, Av1Parameters>;

inline bool IsVideoCodec(CodecType codec) noexcept
{
    return codec == CodecType::H264 || codec == CodecType::H265 || codec == CodecType::VP8 ||
        codec == CodecType::VP9 || codec == CodecType::AV1;
}

inline EncodedBitstream BitstreamFor(CodecType codec) noexcept
{
    switch (codec) {
    case CodecType::H264:
    case CodecType::H265: return EncodedBitstream::AnnexB;
    case CodecType::VP8: return EncodedBitstream::Vp8Frame;
    case CodecType::VP9: return EncodedBitstream::Vp9Frame;
    case CodecType::AV1: return EncodedBitstream::Av1Obu;
    default: return EncodedBitstream::Unspecified;
    }
}

inline bool ParametersMatch(CodecType codec, const VideoCodecParameters& parameters) noexcept
{
    if (!IsVideoCodec(codec)) return false;
    if (std::holds_alternative<std::monostate>(parameters)) return true;
    switch (codec) {
    case CodecType::H264: return std::holds_alternative<H264Parameters>(parameters);
    case CodecType::H265: return std::holds_alternative<H265Parameters>(parameters);
    case CodecType::VP8: return std::holds_alternative<Vp8Parameters>(parameters);
    case CodecType::VP9: return std::holds_alternative<Vp9Parameters>(parameters);
    case CodecType::AV1: return std::holds_alternative<Av1Parameters>(parameters);
    default: return false;
    }
}

struct VideoDecoderConfig { CodecType codec = CodecType::H264; };

struct VideoEncoderConfig
{
    CodecType codec = CodecType::H264;
    int width = 0, height = 0;
    int bitrate_bps = 0;
    int frame_rate = 30;
    int keyframe_interval = 60;
    VideoCodecParameters parameters;
};

enum class CodecDirection { Decode, Encode };
struct VideoCodecCapability
{
    CodecType codec = CodecType::Unknown;
    CodecDirection direction = CodecDirection::Decode;
    std::string backend;
    std::string implementation;
    bool built_in = false;
    // Library discovery is not an initialization/configuration guarantee.
    bool library_available = false;
};

} // namespace media::codec
