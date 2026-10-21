#pragma once

#include "MediaFrame.h"
#include "VideoCodecConfig.h"

#include <array>
#include <memory>
#include <string>

namespace media::codec {

enum class PixelLayout { I420, I422, I444, NV12, Gray };
enum class ColorMatrix { Unspecified, Bt709, Bt601, Smpte240M, Bt2020 };

struct VideoFrameStorage { virtual ~VideoFrameStorage() = default; };

// Immutable CPU plane views. storage owns ALL planes; views remain valid while
// it is retained. Strides are bytes and may be negative. No FFmpeg types escape.
struct RawVideoFrame
{
    std::shared_ptr<const VideoFrameStorage> storage;
    std::array<const uint8_t*, 4> planes{};
    std::array<int, 4> strides{};
    int width = 0, height = 0;
    PixelLayout layout = PixelLayout::I420;
    int bit_depth = 8;
    bool big_endian = false;
    bool full_range = false;
    ColorMatrix matrix = ColorMatrix::Unspecified;
    int sar_num = 1, sar_den = 1;
    FrameTimestamp timestamp;
};

} // namespace media::codec
