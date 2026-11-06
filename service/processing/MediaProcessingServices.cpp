#include "MediaProcessingServices.h"

#include "AIService.h"
#include "UnavailableModelProvider.h"
#include "RecordingService.h"
#include "ConferenceMixService.h"
#include "TranscodeService.h"

#include <stdexcept>
#include <utility>

namespace service {

MediaProcessingServices CreateLocalMediaProcessingServices(
    std::shared_ptr<media::EncodedFrameRouter> input,
    std::shared_ptr<config::ConfigStore> settings,
    std::shared_ptr<IRecordingEventSink> recording_events)
{
    if (!input || !settings)
        throw std::invalid_argument("Media processing requires a frame router and settings");

    MediaProcessingServices services;
    services.mixed_frames = std::make_shared<media::EncodedFrameRouter>();
    services.transcoded_frames = std::make_shared<media::EncodedFrameRouter>();
    services.recording = std::make_shared<RecordingService>(input, settings->Snapshot()->recording, std::move(recording_events), settings);
    services.ai = std::make_shared<ai::AIService>(std::make_shared<ai::UnavailableModelProvider>(), input, nullptr, 128, settings);
    services.mix = std::make_shared<mix::ConferenceMixService>(input, services.mixed_frames, settings);
    services.transcode = std::make_shared<transcode::TranscodeService>(input, services.transcoded_frames);
    return services;
}

} // namespace service
