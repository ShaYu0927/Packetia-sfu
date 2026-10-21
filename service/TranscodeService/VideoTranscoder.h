#pragma once

#include "IVideoTranscoder.h"

#include <memory>

namespace service::transcode {

class VideoTranscoder final : public IVideoTranscoder
{
public:
    VideoTranscoder();
    ~VideoTranscoder() override;
    VideoTranscoder(const VideoTranscoder&) = delete;
    VideoTranscoder& operator=(const VideoTranscoder&) = delete;

    bool Open(const VideoTranscodeConfig& config = {}) override;

    bool Input(const media::EncodedFrame& frame, std::vector<TranscodedFrame>& output) override;

    bool Flush(std::vector<TranscodedFrame>& output) override;
    void Close() noexcept override;

    const std::string& LastError() const noexcept override;
    VideoTranscodeStats Stats() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace service::transcode
