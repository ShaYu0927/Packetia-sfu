#ifndef _TRACK_CLOCK_H_
#define _TRACK_CLOCK_H_

#include <cstdint>

namespace media
{

// Maintains the RTCP SR mapping between a track's RTP clock and wall time.
// This class only converts timestamps; buffering and A/V frame selection
// belong to a higher-level synchronization group.
class TrackClock
{
public:
    struct Mapping
    {
        bool valid = false;
        int64_t unix_time_us = 0;
    };

    explicit TrackClock(uint32_t clock_rate = 0) : clock_rate_(clock_rate) {}

    void SetClockRate(uint32_t clock_rate);
    uint32_t ClockRate() const { return clock_rate_; }

    bool UpdateSenderReport(uint64_t ntp, uint32_t rtp_timestamp);
    Mapping Map(uint32_t rtp_timestamp) const;
    void Reset();

    bool Valid() const { return valid_; }
    uint64_t LastSenderReportNtp() const { return sr_ntp_; }
    uint32_t LastSenderReportRtpTimestamp() const { return sr_rtp_timestamp_; }

    static bool NtpToUnixTimeUs(uint64_t ntp, int64_t* unix_time_us);

private:
    uint32_t clock_rate_ = 0;
    bool valid_ = false;
    uint64_t sr_ntp_ = 0;
    int64_t sr_unix_time_us_ = 0;
    uint32_t sr_rtp_timestamp_ = 0;
};

} // namespace media

#endif // _TRACK_CLOCK_H_
