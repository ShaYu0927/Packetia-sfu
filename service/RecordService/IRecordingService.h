#pragma once

#include "IService.h"
#include "RecordingRecords.h"

namespace config { struct StreamKey; }

namespace service {

// Application control boundary; frame ingestion and persistence stay private.
class IRecordingService : public IService
{
public:
    // Call from the control thread, never from a recording event/worker callback.
    // Start admits future media; Stop drains admitted frames and finalizes files.
    virtual bool StartRecording(const config::StreamKey& stream) = 0;
    virtual bool StopRecording(const config::StreamKey& stream) = 0;
    virtual std::vector<SegmentInfo> QuerySegments(const SegmentQuery& query) const = 0;
    virtual std::vector<RecordedStream> ListRecordedStreams(const std::string& session_id) const = 0;
    virtual void RefreshStreamPolicies() = 0;
};

} // namespace service
