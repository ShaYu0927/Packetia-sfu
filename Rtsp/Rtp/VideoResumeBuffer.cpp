#include "VideoResumeBuffer.h"
#include <algorithm>

namespace rtsp
{
namespace
{
uint16_t U16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t U32(const uint8_t* p) { return (uint32_t(U16(p)) << 16) | U16(p + 2); }
bool Before(uint16_t a, uint16_t b) { return a != b && uint16_t(b - a) < 0x8000; }
struct PayloadInfo { bool valid = false, key = false, parameter = false; uint8_t type = 0; };
PayloadInfo H264(const uint8_t* p, size_t n)
{
    if (n < 2 || (p[0] & 0x80)) return {};
    const uint8_t type = p[0] & 31;
    if (type >= 1 && type <= 23)
        return {true, type == 5 && bool(p[1] & 0x80), type == 7 || type == 8, type};
    if (type == 28)
    {
        if (n < 3 || (p[1] & 0x20) || (p[1] & 0xc0) == 0xc0 ||
            !(p[1] & 31) || (p[1] & 31) >= 24) return {};
        return {true, (p[1] & 0x80) && (p[1] & 31) == 5 && bool(p[2] & 0x80), false, type};
    }
    if (type != 24) return {};
    bool key = false, parameters = true;
    size_t offset = 1;
    unsigned count = 0;
    while (offset + 2 <= n)
    {
        const auto len = U16(p + offset);
        offset += 2;
        if (len < 2 || len > n - offset) return {};
        if ((p[offset] & 31) >= 24) return {};
        const auto unit = H264(p + offset, len);
        if (!unit.valid || unit.type >= 24) return {};
        key |= unit.key;
        parameters &= unit.parameter;
        offset += len;
        ++count;
    }
    return offset == n && count ? PayloadInfo{true, key, parameters, type} : PayloadInfo{};
}
PayloadInfo H265(const uint8_t* p, size_t n)
{
    if (n < 3 || (p[0] & 0x80) || !(p[1] & 7)) return {};
    const uint8_t type = (p[0] >> 1) & 63;
    if (type < 48)
        return {true, (type == 19 || type == 20) && bool(p[2] & 0x80),
            type >= 32 && type <= 34, type};
    if (type == 49)
    {
        if (n < 4 || (p[2] & 0xc0) == 0xc0 || (p[2] & 63) >= 48) return {};
        return {true, (p[2] & 0x80) && ((p[2] & 63) == 19 || (p[2] & 63) == 20) &&
            bool(p[3] & 0x80), false, type};
    }
    if (type != 48) return {};
    size_t offset = 2;
    bool key = false, parameters = true;
    unsigned count = 0;
    while (offset + 2 <= n)
    {
        const auto len = U16(p + offset);
        offset += 2;
        if (len < 3 || len > n - offset) return {};
        if (((p[offset] >> 1) & 63) >= 48) return {};
        const auto unit = H265(p + offset, len);
        if (!unit.valid || unit.type >= 48) return {};
        key |= unit.key;
        parameters &= unit.parameter;
        offset += len;
        ++count;
    }
    return offset == n && count ? PayloadInfo{true, key, parameters, type} : PayloadInfo{};
}
}

void VideoResumeBuffer::ClearFrame()
{
    timestamp_.reset(); first_.reset(); last_.reset(); key_.reset();
    packets_.clear(); bytes_ = 0;
}
void VideoResumeBuffer::Reset() { ClearFrame(); discarded_timestamp_.reset(); parameters_.clear(); }

VideoResumeBuffer::Frame VideoResumeBuffer::Push(CodecId codec, const uint8_t* data, size_t size, uint64_t now_ms)
{
    if (!data || size < 12 || (data[0] >> 6) != 2) return {};
    size_t offset = 12 + (data[0] & 15) * 4;
    if (offset > size) return {};
    if (data[0] & 0x10)
    {
        if (offset + 4 > size) return {};
        offset += 4 + size_t(U16(data + offset + 2)) * 4;
    }
    size_t end = size;
    if (data[0] & 0x20)
    {
        if (!data[size - 1] || data[size - 1] > size - std::min(size, offset)) return {};
        end -= data[size - 1];
    }
    if (offset >= end) return {};
    const auto info = codec == CodecId::H264 ? H264(data + offset, end - offset) :
        codec == CodecId::H265 ? H265(data + offset, end - offset) : PayloadInfo{};
    if (!info.valid) return {};
    const auto timestamp = U32(data + 4);
    const auto sequence = U16(data + 2);
    if (discarded_timestamp_ && uint32_t(timestamp - *discarded_timestamp_) >= 0x80000000U) return {};
    if (discarded_timestamp_ && timestamp == *discarded_timestamp_) return {};
    if (timestamp_ && timestamp != *timestamp_)
    {
        if (uint32_t(timestamp - *timestamp_) >= 0x80000000U) return {};
        ClearFrame();
    }
    if (info.parameter && size <= 16384)
    {
        parameters_.erase(std::remove_if(parameters_.begin(), parameters_.end(),
            [&](const auto& entry) { return entry.first == info.type; }), parameters_.end());
        parameters_.emplace_back(info.type, Packet(data, data + size));
    }
    if (timestamp_ && (now_ms < started_ms_ || now_ms - started_ms_ >= 200))
    {
        discarded_timestamp_ = timestamp_;
        ClearFrame();
        return {};
    }
    if (!timestamp_) { timestamp_ = timestamp; started_ms_ = now_ms; }
    if (packets_.size() >= 512 || bytes_ + size > 2 * 1024 * 1024)
    { discarded_timestamp_ = timestamp_; ClearFrame(); return {}; }
    if (packets_.emplace(sequence, Packet(data, data + size)).second) bytes_ += size;
    if (info.key) key_ = sequence;
    if ((info.key || info.parameter) && (!first_ || Before(sequence, *first_))) first_ = sequence;
    if (data[1] & 0x80) last_ = sequence;
    if (!key_ || !first_ || !last_) return {};
    const auto count = uint16_t(*last_ - *first_) + 1U;
    if (count > 512 || uint16_t(*key_ - *first_) >= count) return {};
    for (unsigned i = 0; i < count; ++i)
        if (!packets_.count(uint16_t(*first_ + i))) return {};

    Frame result;
    // Parameter-only packets from earlier timestamps may have changed while
    // paused. Replay them in the recovery AU with synthetic preceding sequence
    // numbers; all real source packets keep their relative sequence spacing.
    for (const auto& parameter : parameters_)
    {
        bool included = false;
        for (const auto& packet : packets_)
            if (packet.second == parameter.second) { included = true; break; }
        if (!included) result.push_back(parameter.second);
    }
    for (size_t i = 0; i < result.size(); ++i)
    {
        auto& packet = result[i];
        const auto seq = uint16_t(*first_ - result.size() + i);
        packet[2] = uint8_t(seq >> 8); packet[3] = uint8_t(seq);
        std::copy(data + 4, data + 8, packet.begin() + 4);
        packet[1] &= 0x7f;
    }
    for (unsigned i = 0; i < count; ++i) result.push_back(std::move(packets_.at(uint16_t(*first_ + i))));
    ClearFrame();
    return result;
}
}
