#pragma once

#include "IRecordingService.h"
#include "IConferenceMixService.h"
#include "ITranscodeService.h"

#include <functional>
#include <memory>

namespace config { class ConfigStore; }
namespace media { class EncodedFrameRouter; }

namespace service {
class IRecordingEventSink;

// Dependencies supplied to the application. It controls services through these
// interfaces and keeps the same lifecycle owner as the network services.
struct MediaProcessingServices
{
    std::shared_ptr<IRecordingService> recording;
    std::shared_ptr<IService> ai;
    std::shared_ptr<mix::IConferenceMixService> mix;
    std::shared_ptr<transcode::ITranscodeService> transcode;
    std::shared_ptr<media::EncodedFrameRouter> mixed_frames;
    std::shared_ptr<media::EncodedFrameRouter> transcoded_frames;
};

// Invoked once during application construction. Return a complete set of fresh,
// unstarted services bound to the supplied input and shared settings. The caller
// owns lifecycle; factories must not start workers or retain the application.
using MediaProcessingFactory = std::function<MediaProcessingServices(
    std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<config::ConfigStore> settings,
    std::shared_ptr<IRecordingEventSink> recording_events)>;

// Composition only: allocates local services without starting threads or tasks.
// The caller registers lifecycle in dependency order and serializes control calls.
MediaProcessingServices CreateLocalMediaProcessingServices(
    std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<config::ConfigStore> settings,
    std::shared_ptr<IRecordingEventSink> recording_events = {});

} // namespace service
