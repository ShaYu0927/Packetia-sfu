#ifndef _MEDIA_STREAM_AFFINITY_H_
#define _MEDIA_STREAM_AFFINITY_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "ShardedWorkerPool.h"

namespace media_affinity
{

struct MediaStreamHandle
{
    uint64_t endpoint_id = 0;
    uint32_t ssrc = 0;
    uint64_t affinity_key = 0;
};

inline uint16_t ReadUint16BE(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

inline uint32_t ReadUint32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

inline bool TryGetRtpSsrc(const uint8_t* data, size_t len, uint32_t& ssrc)
{
    if (!data || len < 12 || (data[0] >> 6) != 2)
        return false;
    ssrc = ReadUint32BE(data + 8);
    return true;
}

// Returns the media SSRC used to serialize RTCP with its RTP stream. Feedback
// packets and report blocks take precedence over the RTCP sender SSRC.
inline bool TryGetRtcpMediaSsrc(const uint8_t* data, size_t len, uint32_t& ssrc)
{
    if (!data || len < 4)
        return false;

    size_t offset = 0;
    bool has_fallback = false;
    uint32_t fallback = 0;
    while (offset + 4 <= len)
    {
        const uint8_t* packet = data + offset;
        if ((packet[0] >> 6) != 2)
            return false;
        const uint8_t count_or_fmt = packet[0] & 0x1F;
        const uint8_t packet_type = packet[1];
        const size_t packet_len = (static_cast<size_t>(ReadUint16BE(packet + 2)) + 1) * 4;
        if (packet_len < 4 || offset + packet_len > len)
            return false;

        if ((packet_type == 205 || packet_type == 206) && packet_len >= 12)
        {
            ssrc = ReadUint32BE(packet + 8);
            return true;
        }
        if (packet_type == 201 && count_or_fmt > 0 && packet_len >= 32)
        {
            ssrc = ReadUint32BE(packet + 8);
            return true;
        }
        if (packet_type == 200 && count_or_fmt > 0 && packet_len >= 52)
        {
            ssrc = ReadUint32BE(packet + 28);
            return true;
        }
        if (!has_fallback &&
            (packet_type == 200 || packet_type == 201 || packet_type == 203) &&
            packet_len >= 8)
        {
            fallback = ReadUint32BE(packet + 4);
            has_fallback = true;
        }
        offset += packet_len;
    }

    if (offset != len || !has_fallback)
        return false;
    ssrc = fallback;
    return true;
}

inline uint64_t MakeStreamKey(uint64_t endpoint_id, uint32_t ssrc)
{
    // SplitMix64 finalizer gives stable distribution without truncating the
    // 64-bit endpoint id. SSRC zero still gets an endpoint-specific key.
    uint64_t value = endpoint_id ^
        (static_cast<uint64_t>(ssrc) + 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline MediaStreamHandle MakeStreamHandle(uint64_t endpoint_id, uint32_t ssrc)
{
    return MediaStreamHandle{endpoint_id, ssrc, MakeStreamKey(endpoint_id, ssrc)};
}

// All mutable state belonging to one media stream must be accessed from tasks
// posted through this function. The stable affinity key makes one media worker
// the owner of the (endpoint, SSRC) pair and preserves FIFO ordering.
inline int PostToMediaStream(const MediaStreamHandle& stream, std::function<void()> fn)
{
    if (stream.endpoint_id == 0 || !fn)
        return -1;
    return WorkerService::post_fn("media", stream.affinity_key, std::move(fn));
}

inline int PostToMediaStream(uint64_t endpoint_id, uint32_t ssrc, std::function<void()> fn)
{
    return PostToMediaStream(MakeStreamHandle(endpoint_id, ssrc), std::move(fn));
}

inline uint64_t ResolveRtpKey(uint64_t endpoint_id, const uint8_t* data, size_t len)
{
    uint32_t ssrc = 0;
    return TryGetRtpSsrc(data, len, ssrc) ? MakeStreamKey(endpoint_id, ssrc) : endpoint_id;
}

inline uint64_t ResolveRtcpKey(uint64_t endpoint_id, const uint8_t* data, size_t len)
{
    uint32_t ssrc = 0;
    return TryGetRtcpMediaSsrc(data, len, ssrc) ? MakeStreamKey(endpoint_id, ssrc) : endpoint_id;
}

} // namespace media_affinity

#endif // _MEDIA_STREAM_AFFINITY_H_
