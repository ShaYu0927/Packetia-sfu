#ifndef _MP4_H_
#define _MP4_H_

#include <memory>
#include <string>
#include <vector>

#include "TrackeInfo.h"
#include "MediaFrame.h"

namespace service
{
    
struct StreamKey
{
    std::string app;
    std::string stream_id;

    bool operator==(const StreamKey& other) const
    {
        return app == other.app && stream_id == other.stream_id;
    }
};

struct RecordingRequest
{
    StreamKey stream_key;
    std::string output_directory;
    std::string file_name;
    bool wait_for_keyframe = true;
    bool prepend_parameter_sets = true;
};

struct Mp4Sample
{
    uint32_t track_id = 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
    int64_t dts = 0;
    int64_t pts = 0;
    uint32_t duration = 0;
    bool key_frame = false;
};


class IMediaSink
{
public:
    virtual ~IMediaSink() = default;

    virtual bool AddTrack(const std::shared_ptr<media::TrackInfo>& track) = 0;

    virtual bool InputFrame(const media::EncodedFrame::Ptr& frame) = 0;

    virtual void ResetTracks() = 0;

    virtual void Flush() = 0;
};

}


#endif /* _MP4_H_ */