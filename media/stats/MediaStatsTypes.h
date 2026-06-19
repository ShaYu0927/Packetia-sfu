#ifndef _MEDIA_STATS_TYPES_H_
#define _MEDIA_STATS_TYPES_H_

#include <cstdint>
#include <string>

namespace media 
{

enum class MediaProtocol
{
    UNKNOWN = 0,
    RTP,
    RTMP,
    HLS,
    RTSP,
    WEBRTC
};

enum class MediaType
{
    UNKNOWN = 0,
    AUDIO,
    VIDEO,
    DATA
};

enum class MediaDirection
{
    UNKNOWN = 0,
    INBOUND,
    OUTBOUND
};





struct MediaStatsKey
{
    MediaProtocol protocol = MediaProtocol::UNKNOWN;
    MediaType media_type = MediaType::UNKNOWN;
    MediaDirection direction = MediaDirection::UNKNOWN;

    std::string session_id;
    std::string stream_id;
    std::string track_id;

    uint32_t ssrc = 0;
    uint32_t connection_id = 0;
};

struct MediaStatsMeta
{
    MediaProtocol protocol = MediaProtocol::UNKNOWN;
    MediaType media_type = MediaType::UNKNOWN;
    MediaDirection direction = MediaDirection::UNKNOWN;

    std::string session_id;
    std::string stream_id;
    std::string track_id;

    uint32_t ssrc = 0;       
    uint32_t clock_rate = 0; 

    std::string codec;      
};

struct MediaTrackStats
{
    MediaStatsMeta meta;

    uint64_t packets_recv = 0;
    uint64_t packets_send = 0;
    uint64_t packets_lost = 0;

    uint64_t bytes_recv = 0;
    uint64_t bytes_send = 0;

    uint16_t last_seq = 0;
    uint32_t last_rtp_timestamp = 0;

    uint32_t jitter = 0;
    uint32_t rtt_ms = 0;

    int64_t create_time_ms = 0;
    int64_t last_event_time_ms = 0;
};

}

#endif /* _MEDIA_STATS_TYPES_H_ */