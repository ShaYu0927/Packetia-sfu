#ifndef _RTPINTERLEAVED_H_
#define _RTPINTERLEAVED_H_

#include "Rtp.h"
#include "PacketPool.h"

#include <optional>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <cstring>

class RtpTrack; 

struct InterleavedBinding
{
    std::weak_ptr<RtpTrack> track;   // 不持有生命周期
    bool is_rtcp = false;            // true=RTCP, false=RTP
};

class InterleavedChannelMap
{
public:
    void bind(uint8_t channel,
              std::weak_ptr<RtpTrack> track,
              bool is_rtcp)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        map_[channel] = InterleavedBinding{std::move(track), is_rtcp};
    }

    void unbind(uint8_t channel)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.erase(channel);
    }

    
    std::optional<InterleavedBinding> get(uint8_t channel) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(channel);
        if (it == map_.end())
            return std::nullopt;
        return it->second;
    }

   
    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::unordered_map<uint8_t, InterleavedBinding> map_;
};



class RtpInterleaved
{
public:
    void bind(uint8_t ch, std::weak_ptr<RtpTrack> track, bool is_rtcp);
    void unbind(uint8_t ch);

    int onInterleaved(uint8_t channel,
                      const uint8_t* payload,
                      size_t length);

    void SetPacketPool(PacketPool* pool) { pool_ = pool; }

private:
    InterleavedChannelMap map_;
    PacketPool* pool_ = nullptr;
};




#endif //_RTPINTERLEAVED_H_