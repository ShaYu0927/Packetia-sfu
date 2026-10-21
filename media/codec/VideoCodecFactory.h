#pragma once

#include "IVideoCodec.h"

namespace media::codec {

// Only registered adapters are discoverable, even if FFmpeg has other codecs.
// Auto uses registration order; Prefer tries a named implementation first;
// Require never falls back. Unavailable/failed candidates are released before
// trying the next implementation of the SAME codec. Returns OPENED instances.
class VideoCodecFactory
{
public:
    static std::vector<VideoCodecCapability> Capabilities();
    static std::unique_ptr<IVideoDecoder> CreateDecoder(
        const VideoDecoderConfig& config, const BackendRequest& request, CodecError& error);
    static std::unique_ptr<IVideoEncoder> CreateEncoder(
        const VideoEncoderConfig& config, const BackendRequest& request, CodecError& error);
    static std::unique_ptr<IVideoFrameScaler> CreateScaler(int width, int height, CodecError& error);
};

} // namespace media::codec
