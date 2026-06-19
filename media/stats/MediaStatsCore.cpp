#include "MediaStatsCore.h"
#include "TimeUtil.h"

namespace media 
{

MediaStatsHandle MediaStatsCore::RegisterTrack(const MediaStatsMeta& meta)
{
    MediaStatsHandle handle = ++next_handle_;
    if (handle == kInvalidMediaStatsHandle)
    {
        handle = ++next_handle_;
    }

    MediaTrackStats stats;
    stats.meta = meta;
    stats.create_time_ms = Timestamp::NowMs();
    stats.last_event_time_ms = stats.create_time_ms;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_[handle] = stats;
    }

    return handle;
}

void MediaStatsCore::UnregisterTrack(MediaStatsHandle handle)
{
    if (handle == kInvalidMediaStatsHandle) 
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.erase(handle);
}

void MediaStatsCore::OnEvent(const MediaStatsEvent& event)
{
    if (event.handle == kInvalidMediaStatsHandle) 
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tracks_.find(event.handle);
    if (it == tracks_.end()) 
    {
        return;
    }

    MediaTrackStats& stats = it->second;
    stats.last_event_time_ms = event.timestamp_ms > 0 ? event.timestamp_ms : Timestamp::NowMs();

    switch (event.protocol)
    {
    case MediaStatsProtocol::Rtp:
        HandleRtpEvent(stats, event);
        break;

    case MediaStatsProtocol::Rtcp:
        HandleRtcpEvent(stats, event);
        break;

    case MediaStatsProtocol::VideoFrame:
        HandleFrameEvent(stats, event);
        break;

    default:
        break;
    }
}

void MediaStatsCore::HandleRtpEvent(MediaTrackStats& stats, const MediaStatsEvent& event)
{

}

void MediaStatsCore::HandleRtcpEvent(MediaTrackStats& stats, const MediaStatsEvent& event)
{

}

void MediaStatsCore::HandleFrameEvent(MediaTrackStats& stats, const MediaStatsEvent& event)
{

}


}