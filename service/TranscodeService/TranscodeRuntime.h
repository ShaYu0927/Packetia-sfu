#pragma once

#include <functional>
#include <memory>

namespace media { class EncodedFrameRouter; }

namespace service::transcode {

class IVideoTranscoder;
class TranscodeSession;
class TranscodeTask;
using KeyFrameRequest = std::function<void()>;
using VideoTranscoderFactory = std::function<std::unique_ptr<IVideoTranscoder>()>;

} // namespace service::transcode
