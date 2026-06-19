#ifndef _MEDIA_STATS_EVENT_H_
#define _MEDIA_STATS_EVENT_H_

#include <cstdint>
#include <memory>
#include <string>

namespace media 
{

using MediaStatsHandle = uint64_t;
static constexpr MediaStatsHandle kInvalidMediaStatsHandle = 0;

enum class MediaStatsProtocol
{
    Unknown = 0,
    Rtp,
    Rtcp,
    Rtmp,
    Hls,
    AudioFrame,
    VideoFrame
};

enum class MediaStatsDirection
{
    Unknown = 0,
    Send,
    Recv
};

enum class MediaStatsEventType
{
    Unknown = 0,

    // RTP
    RtpPacketRecv,
    RtpPacketSend,
    RtpPacketLost,
    RtpPacketRetransmit,

    // RTCP
    RtcpSr,
    RtcpRr,
    RtcpNack,
    RtcpPli,
    RtcpFir,

    // RTMP
    RtmpChunkRecv,
    RtmpChunkSend,
    RtmpMessageRecv,
    RtmpMessageSend,

    // HLS
    HlsSegmentCreate,
    HlsSegmentSend,
    HlsPlaylistUpdate,

    // Audio / Video frame
    AudioFrameDecoded,
    AudioFrameEncoded,
    VideoFrameDecoded,
    VideoFrameEncoded
};

struct IMediaStatsPayload
{
    virtual ~IMediaStatsPayload() = default;
};

struct RtpStatsPayload : public IMediaStatsPayload
{
    uint32_t ssrc = 0;
    uint16_t seq = 0;
    uint32_t rtp_timestamp = 0;
    uint8_t payload_type = 0;
    bool marker = false;
    uint32_t bytes = 0;
};

struct RtcpStatsPayload : public IMediaStatsPayload
{
    uint32_t ssrc = 0;

    uint8_t fraction_lost = 0;
    uint32_t cumulative_lost = 0;
    uint32_t jitter = 0;

    uint32_t rtt_ms = 0;
};

struct RtmpStatsPayload : public IMediaStatsPayload
{
    uint32_t chunk_stream_id = 0;
    uint32_t message_stream_id = 0;
    uint8_t message_type = 0;
    uint32_t timestamp = 0;
    uint32_t bytes = 0;
};

struct HlsStatsPayload : public IMediaStatsPayload
{
    std::string segment_name;
    uint32_t duration_ms = 0;
    uint32_t size_bytes = 0;
    uint32_t sequence = 0;
};

struct MediaStatsEvent
{
    MediaStatsHandle handle = kInvalidMediaStatsHandle;

    MediaStatsProtocol protocol = MediaStatsProtocol::Unknown;
    MediaStatsDirection direction = MediaStatsDirection::Unknown;
    MediaStatsEventType type = MediaStatsEventType::Unknown;

    int64_t timestamp_ms = 0;
    std::shared_ptr<IMediaStatsPayload> payload;
};

}



#endif /* _MEDIA_STATS_EVENT_H_ */