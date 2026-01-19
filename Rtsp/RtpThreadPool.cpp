#include "RtpThreadPool.h"



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

    Packet* pkt = static_cast<Packet*>(job.payload);
    if (!pkt || !pkt->data || pkt->len == 0) 
    {
        LOG_ERROR("RtpJobHandler::handle: invalid packet");
        return;
    }
    
    const uint8_t* mem = pkt->data;
    size_t len = pkt->len;

    int rc = 0;
    const bool is_rtcp = (job.type == 1) || (pkt->flags == 1);
    if (is_rtcp) 
    {
        if (job.deleter) job.deleter(job);
        return;
    }
    
    
    const auto pt = track->getType();
    const auto sr = track->getSampleRate();
    (void)track->inputRtp(pt, sr, const_cast<uint8_t*>(mem), len);
}
