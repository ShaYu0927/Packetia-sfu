#ifndef _MEDIA_STATS_CORE_H_
#define _MEDIA_STATS_CORE_H_

#include "MediaStatsTypes.h"
#include "MediaStatsEvent.h"

#include <atomic>
#include <mutex>
#include <unordered_map>


namespace media
{
class IMediaStatsSink
{
public:
    virtual ~IMediaStatsSink() = default;

    virtual MediaStatsHandle RegisterTrack(const MediaStatsMeta& meta) = 0;

    virtual void UnregisterTrack(MediaStatsHandle handle) = 0;

    virtual void OnEvent(const MediaStatsEvent& event) = 0;
};

class MediaStatsCore : public IMediaStatsSink
{
public:
    MediaStatsHandle RegisterTrack(const MediaStatsMeta& meta) override;

    void UnregisterTrack(MediaStatsHandle handle) override;

    void OnEvent(const MediaStatsEvent& event) override;

private:
    void HandleRtpEvent(MediaTrackStats& stats, const MediaStatsEvent& event);
    void HandleRtcpEvent(MediaTrackStats& stats, const MediaStatsEvent& event);
    void HandleFrameEvent(MediaTrackStats& stats, const MediaStatsEvent& event);

private:
    std::atomic<uint64_t> next_handle_;
    std::mutex mutex_;
    std::unordered_map<MediaStatsHandle, MediaTrackStats> tracks_;
};

}
#endif /* _MEDIA_STATS_CORE_H_ */