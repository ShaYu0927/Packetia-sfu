#ifndef _CORE_EVENT_H_
#define _CORE_EVENT_H_

#include <cstdint>
#include <string>

#include "stats/MediaStatsTypes.h"

namespace media 
{

enum class MediaEventType
{
    Unknown = 0,

    RtpPacketReceived,
    RtpPacketSent,
    RtcpFeedback,

    PacketGapDetected,
    TimestampJumpDetected,
    StreamTimeout,
    StreamStateChanged
};

enum class MediaKind
{
    Unknown = 0,
    Audio,
    Video
};


enum class MediaEventLevel
{
    Debug = 0,
    Info,
    Warning,
    Error
};

struct MediaEventMeta
{
    std::string session_id;
    std::string track_id;

    uint32_t ssrc = 0;
    uint8_t payload_type = 0;

    MediaKind media_kind = MediaKind::Unknown;
    MediaDirection direction = MediaDirection::UNKNOWN;

    int64_t event_time_ms = 0;
};

class MediaEvent
{
public:
    MediaEvent(MediaEventType type, MediaEventLevel level)
        : type_(type),
          level_(level)
    {
    }

    virtual ~MediaEvent()
    {
    }

    MediaEventType Type() const
    {
        return type_;
    }

    MediaEventLevel Level() const
    {
        return level_;
    }

    MediaEventMeta& Meta()
    {
        return meta_;
    }

    const MediaEventMeta& Meta() const
    {
        return meta_;
    }

    void SetReason(const std::string& reason)
    {
        reason_ = reason;
    }

    const std::string& Reason() const
    {
        return reason_;
    }

    virtual const char* Name() const
    {
        return "MediaEvent";
    }

private:
    MediaEventType type_ = MediaEventType::Unknown;
    MediaEventLevel level_ = MediaEventLevel::Info;
    MediaEventMeta meta_;
    std::string reason_;
};

}

#endif /* _CORE_EVENT_H_ */
