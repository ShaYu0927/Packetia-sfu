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
    auto *mem = static_cast<uint8_t *>(job.payload);
    size_t len = job.payload_len;

    if(len == 0 || mem == nullptr)
    {
        LOG_ERROR("RtpJobHandler::handle: invalid job payload");
        return;
    }

    std::shared_ptr<RtpTrack> track;
    {
        std::shared_lock<std::shared_mutex> lk(shared_mtx_);
        auto it = tracks_.find(job.key);
        if (it != tracks_.end())
        {
            track = it->second.lock();
            if (!track)
            {
                tracks_.erase(it);
                LOG_ERROR("RtpJobHandler::handle: track expired, key=", job.key, ", len=", len);
                return;
            }
        }
    }

    if(!track)
    {
        // track 过期：需要写锁清理
        {
            std::unique_lock<std::shared_mutex> lk(shared_mtx_);
            auto it = tracks_.find(job.key);
            if (it != tracks_.end() && it->second.expired()) 
            {
                tracks_.erase(it);
            }
        }
        LOG_ERROR("RtpJobHandler::handle: no track bound for key=", job.key);
        return;
    }

    int rc = 0;
    const bool is_rtcp = (job.type == 1);
    if(is_rtcp)
    {
        // rc = track->onRtpPacket(mem->data, mem->len, mem->recv_ts);
    }
    
    const auto pt = track->getType();
    const auto sr = track->getSampleRate();
    
    RtpPacket::Ptr rtp_pkt = track->inputRtp(pt, sr, mem, len);

    (void)rc;
}
