#include "VideoCodecFactory.h"
#include "FfmpegVideoCodec.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <utility>

namespace media::codec {
namespace {
struct Adapter
{
    CodecType codec;
    CodecDirection direction;
    const char* name;
    AVCodecID av_id;
};
// This is an allowlist of adapted software implementations, not an enumeration
// of everything installed in FFmpeg. Order defines the Auto preference.
constexpr Adapter kAdapters[]{
    {CodecType::H264, CodecDirection::Decode, "h264", AV_CODEC_ID_H264},
    {CodecType::H265, CodecDirection::Decode, "hevc", AV_CODEC_ID_HEVC},
    {CodecType::VP8, CodecDirection::Decode, "vp8", AV_CODEC_ID_VP8},
    {CodecType::VP8, CodecDirection::Decode, "libvpx", AV_CODEC_ID_VP8},
    {CodecType::VP9, CodecDirection::Decode, "vp9", AV_CODEC_ID_VP9},
    {CodecType::VP9, CodecDirection::Decode, "libvpx-vp9", AV_CODEC_ID_VP9},
    {CodecType::AV1, CodecDirection::Decode, "libdav1d", AV_CODEC_ID_AV1},
    {CodecType::AV1, CodecDirection::Decode, "libaom-av1", AV_CODEC_ID_AV1},
    {CodecType::H264, CodecDirection::Encode, "libx264", AV_CODEC_ID_H264},
    {CodecType::H265, CodecDirection::Encode, "libx265", AV_CODEC_ID_HEVC},
    {CodecType::VP8, CodecDirection::Encode, "libvpx", AV_CODEC_ID_VP8},
    {CodecType::VP9, CodecDirection::Encode, "libvpx-vp9", AV_CODEC_ID_VP9},
    {CodecType::AV1, CodecDirection::Encode, "libsvtav1", AV_CODEC_ID_AV1},
    {CodecType::AV1, CodecDirection::Encode, "libaom-av1", AV_CODEC_ID_AV1},
};

bool Available(const Adapter& adapter)
{
    const auto* codec = adapter.direction == CodecDirection::Encode
        ? avcodec_find_encoder_by_name(adapter.name) : avcodec_find_decoder_by_name(adapter.name);
    return codec && codec->id == adapter.av_id && !(codec->capabilities & AV_CODEC_CAP_HARDWARE);
}

std::vector<const Adapter*> Candidates(CodecType codec, CodecDirection direction,
                                      const BackendRequest& request, CodecError& error)
{
    std::vector<const Adapter*> result;
    if (!request.Valid()) {
        error = {CodecErrorCode::InvalidConfiguration, "invalid codec selection mode or missing required implementation"};
        return result;
    }
    for (const auto& adapter : kAdapters) {
        if (adapter.codec == codec && adapter.direction == direction &&
            (request.mode != SelectionMode::Require || request.implementation == adapter.name))
            result.push_back(&adapter);
    }
    if (result.empty()) {
        error = {CodecErrorCode::Unsupported, "no registered implementation for requested codec/direction: " + request.implementation};
    } else if (request.mode == SelectionMode::Prefer) {
        std::stable_partition(result.begin(), result.end(), [&](const Adapter* adapter) {
            return request.implementation == adapter->name;
        });
    }
    return result;
}

bool Validate(const VideoEncoderConfig& config, CodecError& error)
{
    if (!ParametersMatch(config.codec, config.parameters) || config.width < 2 || config.width > 4096 ||
        (config.width & 1) || config.height < 2 || config.height > 4096 || (config.height & 1) ||
        config.bitrate_bps <= 0 || config.bitrate_bps > 100000000 ||
        config.frame_rate <= 0 || config.frame_rate > 240 || config.keyframe_interval <= 0) {
        error = {CodecErrorCode::InvalidConfiguration, "invalid video encoder configuration or mismatched codec parameters"};
        return false;
    }
    if (const auto* h264 = std::get_if<H264Parameters>(&config.parameters)) {
        if (h264->profile != H264Profile::Baseline && h264->profile != H264Profile::Main && h264->profile != H264Profile::High) {
            error = {CodecErrorCode::InvalidConfiguration, "invalid H264 profile"};
            return false;
        }
    }
    const auto* vp9 = std::get_if<Vp9Parameters>(&config.parameters);
    const auto* av1 = std::get_if<Av1Parameters>(&config.parameters);
    if ((vp9 && vp9->profile != 0) || (av1 && av1->profile != 0)) {
        error = {CodecErrorCode::Unsupported, "only 8-bit I420 VP9/AV1 profile 0 is adapted"};
        return false;
    }
    return true;
}

template<typename Interface, typename Config, typename Create>
std::unique_ptr<Interface> CreateSelected(const Config& config, CodecDirection direction,
    const BackendRequest& request, CodecError& error, Create create)
{
    error = {};
    const auto candidates = Candidates(config.codec, direction, request, error);
    if (candidates.empty()) return nullptr;
    std::string failures;
    auto code = CodecErrorCode::Unsupported;
    for (const auto* candidate : candidates) {
        CodecError attempt;
        if (Available(*candidate)) {
            auto instance = create(config, candidate->name, attempt);
            if (instance) { error = {}; return instance; }
            code = attempt.code;
        } else attempt = {CodecErrorCode::Unsupported, "library implementation unavailable"};
        if (!failures.empty()) failures += "; ";
        failures += std::string(candidate->name) + ": " + attempt.message;
    }
    error = {code, std::move(failures)};
    return nullptr;
}
} // namespace

std::vector<VideoCodecCapability> VideoCodecFactory::Capabilities()
{
    std::vector<VideoCodecCapability> result;
    for (const auto& adapter : kAdapters)
        result.push_back({adapter.codec, adapter.direction, "ffmpeg", adapter.name, true, Available(adapter)});
    return result;
}

std::unique_ptr<IVideoDecoder> VideoCodecFactory::CreateDecoder(
    const VideoDecoderConfig& config, const BackendRequest& request, CodecError& error)
{
    return CreateSelected<IVideoDecoder>(config, CodecDirection::Decode, request, error, CreateFfmpegDecoder);
}

std::unique_ptr<IVideoEncoder> VideoCodecFactory::CreateEncoder(
    const VideoEncoderConfig& config, const BackendRequest& request, CodecError& error)
{
    error = {};
    if (!IsVideoCodec(config.codec)) {
        error = {CodecErrorCode::Unsupported, "no video encoder for requested codec"};
        return nullptr;
    }
    if (!Validate(config, error)) return nullptr;
    return CreateSelected<IVideoEncoder>(config, CodecDirection::Encode, request, error, CreateFfmpegEncoder);
}

} // namespace media::codec
