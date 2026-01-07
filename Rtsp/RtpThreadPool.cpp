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
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = tracks_.find(job.key);
        if (it != tracks_.end())
        {
            track = it->second.lock();
            if (!track)
            {
                tracks_.erase(it);
                return;
            }
        }
    }

    if(!track)
    {
        LOG_ERROR("RtpJobHandler::handle: no track bound for key=", job.key);
        return;
    }

    int rc = 0;
    const bool is_rtcp = (job.type == 1); // 假设 type 1 表示 RTCP
    if(is_rtcp)
    {
        // rc = track->onRtpPacket(mem->data, mem->len, mem->recv_ts);
    }
    else
    {
        RtpPacket::Ptr rtp_pkt = track->inputRtp(track->getType(), track->getSampleRate(), mem, len);
        if(rtp_pkt)
        {
            // 处理 RTP 包，例如传递给下游处理器
        }
    }
    (void)rc;
}
