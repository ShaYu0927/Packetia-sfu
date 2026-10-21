#pragma once

#include <cstdint>

namespace media {

using TrackId = uint32_t;
enum class MediaType { Unknown = 0, Audio, Video };
// Append values to preserve the identities used by existing tracks.
enum class CodecType { Unknown = 0, H264, H265, Opus, PCMU, PCMA, AAC, VP8, VP9, AV1 };
enum class EncodedBitstream { Unspecified = 0, AnnexB, Vp8Frame, Vp9Frame, Av1Obu };
enum class FrameIntegrity { Complete = 0, Incomplete, Corrupted };
enum class EncodedFrameType { Unknown = 0, Key, Delta, Config, Audio };

} // namespace media
