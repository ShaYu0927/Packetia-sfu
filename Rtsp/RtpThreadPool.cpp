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

static inline std::string hex16(const uint8_t* p, size_t n)
{
    static const char* H = "0123456789ABCDEF";
    std::string s;
    size_t m = (n < 16 ? n : 16);
    s.reserve(m * 3);
    for (size_t i = 0; i < m; ++i)
    {
        unsigned v = p[i];
        s.push_back(H[v >> 4]);
        s.push_back(H[v & 0xF]);
        s.push_back(i + 1 == m ? '\0' : ' ');
    }
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
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

    LOG_INFO(std::string("[RtpJob] recv len=") + std::to_string(len) +
         " is_rtcp=" + std::to_string((int)is_rtcp) +
         " head16=" + hex16(mem, len));


    if (!mem || len < 12) 
    { 
        return;
    }

    if (len >= 4 && mem[0] == 0x24) 
    {
        uint8_t ch = mem[1];
        uint16_t l = (uint16_t(mem[2]) << 8) | uint16_t(mem[3]);
        LOG_INFO(std::string("[RtpJob] RTSP interleaved detected: ch=") + std::to_string((int)ch) +
                " declared_len=" + std::to_string(l) +
                " buf_len=" + std::to_string(len));
    }
    
    const auto pt = track->getType();
    const auto sr = track->getSampleRate();
    if(!is_rtcp)  /* rtp */
    {
        auto RtpPkt = track->inputRtp(pt, sr, const_cast<uint8_t*>(mem), len);
        if (!RtpPkt) 
        {
            return;
        }
    }
    else        /* rtcp */
    {
        track->inputRtcp(const_cast<uint8_t*>(mem), len);
    }
   
    if (job.deleter) job.deleter(job);

    
    // track->inputPacket(rtp);
}
