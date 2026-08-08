#include "TrackClock.h"

#include <limits>

namespace media
{
namespace
{
constexpr uint64_t kNtpUnixEpochOffsetSeconds = 2208988800ULL;
constexpr uint64_t kMicrosPerSecond = 1000000ULL;
}

void TrackClock::SetClockRate(uint32_t clock_rate)
{
    if (clock_rate_ != clock_rate)
    {
        clock_rate_ = clock_rate;
        valid_ = false;
    }
}

bool TrackClock::NtpToUnixTimeUs(uint64_t ntp, int64_t* unix_time_us)
{
    if (!unix_time_us)
    {
        return false;
    }

    const uint64_t seconds = ntp >> 32;
    if (seconds < kNtpUnixEpochOffsetSeconds)
    {
        return false;
    }

    const uint64_t unix_seconds = seconds - kNtpUnixEpochOffsetSeconds;
    const uint64_t fraction = ntp & 0xFFFFFFFFULL;
    const uint64_t fraction_us = (fraction * kMicrosPerSecond) >> 32;
    if (unix_seconds >
        (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - fraction_us) /
            kMicrosPerSecond)
    {
        return false;
    }

    *unix_time_us = static_cast<int64_t>(unix_seconds * kMicrosPerSecond + fraction_us);
    return true;
}

bool TrackClock::UpdateSenderReport(uint64_t ntp, uint32_t rtp_timestamp)
{
    int64_t unix_time_us = 0;
    if (clock_rate_ == 0 || !NtpToUnixTimeUs(ntp, &unix_time_us))
    {
        return false;
    }

    sr_ntp_ = ntp;
    sr_unix_time_us_ = unix_time_us;
    sr_rtp_timestamp_ = rtp_timestamp;
    valid_ = true;
    return true;
}

TrackClock::Mapping TrackClock::Map(uint32_t rtp_timestamp) const
{
    Mapping result;
    if (!valid_ || clock_rate_ == 0)
    {
        return result;
    }

    const int32_t delta = static_cast<int32_t>(rtp_timestamp - sr_rtp_timestamp_);
    const int64_t delta_us =
        static_cast<int64_t>(delta) * static_cast<int64_t>(kMicrosPerSecond) /
        static_cast<int64_t>(clock_rate_);

    result.valid = true;
    result.unix_time_us = sr_unix_time_us_ + delta_us;
    return result;
}

void TrackClock::Reset()
{
    valid_ = false;
    sr_ntp_ = 0;
    sr_unix_time_us_ = 0;
    sr_rtp_timestamp_ = 0;
}

} // namespace media
