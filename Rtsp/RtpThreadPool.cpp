#include "RtpThreadPool.h"

#include <variant>
#include <memory>
#include <utility>


void RtpJobHandler::bind(std::uint64_t key, std::weak_ptr<RtpTrack> track)
{
    std::lock_guard<std::mutex> lock(mtx_);
    tracks_[key] = std::move(track);
}

void RtpJobHandler::unbind(std::uint64_t key)
{
    std::lock_guard<std::mutex> lock(mtx_);
    tracks_.erase(key);
}

void RtpJobHandler::handle(WorkJob &&job)
{
    auto track = job.track.lock();
    if (!track) 
    {
        LOG_ERROR("RtpJobHandler::handle: track expired");
        if (job.deleter) job.deleter(job);
        return;
    }
    const uint8_t* mem = nullptr;
    size_t len = 0;
    bool is_rtcp = false;


    if (auto sp = std::get_if<std::shared_ptr<Packet>>(&job.payload)) 
    {
        const auto& pkt_sp = *sp;
        if (!pkt_sp) 
        {
            return;
        }
        mem = pkt_sp->data;
        len = pkt_sp->len;
        is_rtcp = (pkt_sp->flags & 0x1) != 0;  
    }
    else if (auto pp = std::get_if<std::pair<unsigned char*, size_t>>(&job.payload)) 
    {
        mem = pp->first;
        len = pp->second;
        is_rtcp = (job.type == 1);
    }
    else
    {
        return;
    }

    if (!mem || len < 12) 
    { 
        return;
    }
    
    const auto pt = track->getType();
    const auto sr = track->getSampleRate();
    if(!is_rtcp)
    {
        auto RtpPkt = track->inputRtp(pt, sr, const_cast<uint8_t*>(mem), len);
        if (!RtpPkt) 
        {
            return;
        }
    }
    else
    {
        track->inputRtcp(const_cast<uint8_t*>(mem), len);
    }
   
    if (job.deleter) job.deleter(job);

    
    // track->inputPacket(rtp);
}
