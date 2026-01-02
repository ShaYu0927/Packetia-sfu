#ifndef _RTPTHREADPOOL_H_
#define _RTPTHREADPOOL_H_

#include "ShardedWorkerPool.h"
#include "Rtp.h"
#include "RtpRingBuffer.h"
#include "PacketPool.h"

class RtpJobHandler : public IJobHandler
{
public:
    RtpJobHandler(PacketPool* pool) : pool_(pool) {}

    // 业务侧注册：key(track_id) -> weak_ptr<RtpTrack>
    void bind(std::uint64_t key, std::weak_ptr<RtpTrack> track);

    void unbind(std::uint64_t key);

    void handle(WorkJob&& job) override;

private:
    PacketPool* pool_;

   
    std::mutex mtx_;
    std::unordered_map<std::uint64_t, std::weak_ptr<RtpTrack>> tracks_;
};

#endif /* _RTPTHREADPOOL_H_ */