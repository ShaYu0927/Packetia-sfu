#ifndef _MEDIA_STATS_TYPES_H_
#define _MEDIA_STATS_TYPES_H_

#include <cstdint>
#include <string>
#include "MediaTypes.h"

namespace media 
{

enum class MediaProtocol
{
    Unknown = 0,
    Rtp,
    Rtmp,
    Hls,
    Rtsp,
    WebRtc
};


struct MediaStatsKey
{
    MediaProtocol protocol = MediaProtocol::Unknown;
    MediaType media_type = MediaType::Unknown;
    MediaFlowDirection direction = MediaFlowDirection::Unknown;

    std::string session_id;
    std::string stream_id;
    std::string track_id;

    uint32_t ssrc = 0;
    uint32_t connection_id = 0;
};

struct MediaStatsMeta
{
    MediaProtocol protocol = MediaProtocol::Unknown;
    MediaType media_type = MediaType::Unknown;
    MediaFlowDirection direction = MediaFlowDirection::Unknown;

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
