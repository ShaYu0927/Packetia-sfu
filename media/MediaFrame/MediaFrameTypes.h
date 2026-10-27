#pragma once

#include "MediaTypes.h"

namespace media {

enum class EncodedBitstream { Unspecified = 0, AnnexB, Vp8Frame, Vp9Frame, Av1Obu };
enum class FrameIntegrity { Complete = 0, Incomplete, Corrupted };
enum class EncodedFrameType { Unknown = 0, Key, Delta, Config, Audio };

} // namespace media
