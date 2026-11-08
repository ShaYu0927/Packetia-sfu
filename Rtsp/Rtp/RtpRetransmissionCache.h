#pragma once

#include "SharedBuffer.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rtsp
{
// Owned by one sender track and used on the same serialized execution context.
// The callback borrows the packet only for the duration of the call.
class RtpRetransmissionCache
{
public:
    struct Config
    {
        size_t capacity;
        uint64_t max_age_ms;
        uint64_t min_interval_ms;
        uint32_t max_attempts;
    };
    using Send = std::function<bool(uint16_t, const common::SharedBuffer&)>;

    explicit RtpRetransmissionCache(Config config) : config_(config) {}
    void Store(uint16_t sequence, const uint8_t* data, size_t size, uint64_t now_ms);
    void Clear();
    void Retransmit(const std::vector<uint16_t>& sequences, uint64_t now_ms, const Send& send);

private:
    struct Entry
    {
        common::SharedBuffer packet;
        uint64_t first_send_ms = 0;
        uint64_t last_retransmit_ms = 0;
        uint32_t retransmit_count = 0;
    };
    Config config_;
    std::deque<uint16_t> order_;
    std::unordered_map<uint16_t, Entry> packets_;
};
}
