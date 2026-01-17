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

    auto *mem = static_cast<uint8_t *>(job.payload);
    size_t len = job.payload_len;

    if(len == 0 || mem == nullptr)
    {
        LOG_ERROR("RtpJobHandler::handle: invalid job payload");
        return;
    }

    int rc = 0;
    const bool is_rtcp = (job.type == 1);
    if (is_rtcp) 
    {
        if (job.deleter) job.deleter(job);
        return;
    }
    
    const auto pt = track->getType();
    const auto sr = track->getSampleRate();
    
    RtpPacket::Ptr rtp_pkt = track->inputRtp(pt, sr, mem, len);

    if (job.deleter) job.deleter(job);

    (void)rc;
}
