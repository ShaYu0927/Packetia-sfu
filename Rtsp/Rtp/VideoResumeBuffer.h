#pragma once

#include "RtpTypes.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace rtsp
{
// Used only while a subscription is waiting for recovery. Keeps original RTP
// packets until a bounded, contiguous IDR AU is available, including parameter
// packets. H265 accepts IDR (19/20), not CRA with possible leading pictures.
class VideoResumeBuffer
{
public:
    using Packet = std::vector<uint8_t>;
    using Frame = std::vector<Packet>;
    Frame Push(CodecId codec, const uint8_t* data, size_t size, uint64_t now_ms);
    void Reset();
    void Restart() { ClearFrame(); }
private:
    std::optional<uint32_t> timestamp_;
    std::optional<uint32_t> discarded_timestamp_;
    std::optional<uint16_t> first_, last_, key_;
    uint64_t started_ms_ = 0;
    size_t bytes_ = 0;
    std::map<uint16_t, Packet> packets_;
    // Replay older aggregates before newer individual parameter updates.
    std::vector<std::pair<uint8_t, Packet>> parameters_;
    void ClearFrame();
};
}
