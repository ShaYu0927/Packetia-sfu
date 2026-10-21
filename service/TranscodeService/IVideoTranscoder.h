#pragma once

#include "TranscodedFrame.h"
#include "TranscodeTypes.h"

namespace service::transcode {


class IVideoTranscoder
{
public:
    virtual ~IVideoTranscoder() = default;
    virtual bool Open(const VideoTranscodeConfig& config) = 0;
    virtual bool Input(const media::EncodedFrame& frame, std::vector<TranscodedFrame>& output) = 0;
    virtual bool Flush(std::vector<TranscodedFrame>& output) = 0;
    virtual void Close() noexcept = 0;
    virtual const std::string& LastError() const noexcept = 0;
    virtual VideoTranscodeStats Stats() const noexcept = 0;
};

} // namespace service::transcode
