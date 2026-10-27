#ifndef PACKETIA_MEDIA_STATS_SINK_H_
#define PACKETIA_MEDIA_STATS_SINK_H_

#include "MediaStatsEvent.h"
#include "MediaStatsTypes.h"

namespace media
{
// 生产者只依赖统计输入接口，不持有统计实现、锁或容器。
class IMediaStatsSink
{
public:
    virtual ~IMediaStatsSink() = default;
    virtual MediaStatsHandle RegisterTrack(const MediaStatsMeta& meta) = 0;
    virtual void UnregisterTrack(MediaStatsHandle handle) = 0;
    virtual void OnEvent(const MediaStatsEvent& event) = 0;
};
}

#endif
