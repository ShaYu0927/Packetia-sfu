#pragma once

#include "VideoCodecTypes.h"

namespace media::codec {

// Synchronous, single-owner interfaces: call serially on the task worker.
// Output vectors append owning frames. Close cancels buffered work; Flush
// drains it once and is idempotent. A failed stream must be recreated.
class IVideoDecoder
{
public:
    virtual ~IVideoDecoder() = default;
    virtual bool Decode(const EncodedFrame& frame, std::vector<RawVideoFrame>& output, CodecError& error) = 0;
    virtual bool Flush(std::vector<RawVideoFrame>& output, CodecError& error) = 0;
    virtual void Close() noexcept = 0;
    virtual const char* Implementation() const noexcept = 0;
};

class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;
    virtual bool Encode(const RawVideoFrame& frame, std::vector<EncodedFrame::Ptr>& output, CodecError& error) = 0;
    virtual bool Flush(std::vector<EncodedFrame::Ptr>& output, CodecError& error) = 0;
    virtual void Close() noexcept = 0;
    virtual const char* Implementation() const noexcept = 0;
};

class IVideoFrameScaler
{
public:
    virtual ~IVideoFrameScaler() = default;
    // Fit display aspect ratio onto an exact-size, limited-range I420 canvas.
    virtual bool Scale(const RawVideoFrame& input, RawVideoFrame& output, CodecError& error) = 0;
};

} // namespace media::codec
