#pragma once

#include "IVideoCodec.h"

namespace media::codec {
// Internal backend construction. Public callers use VideoCodecFactory, which
// filters registered names, codec IDs, direction and configuration first.
std::unique_ptr<IVideoDecoder> CreateFfmpegDecoder(const VideoDecoderConfig& config, const std::string& name, CodecError& error);
std::unique_ptr<IVideoEncoder> CreateFfmpegEncoder(const VideoEncoderConfig& config, const std::string& name, CodecError& error);
} // namespace media::codec
