#include "RtpInterleaved.h"

static inline uint16_t ReadE16(const uint8_t* p)
{
    return (uint16_t(p[0] << 8) | uint16_t(p[1]));
}


static inline uint32_t ReadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void HexDumpPrefix(const uint8_t* p,size_t n,size_t max_show = 32)
{
    if (!p || n == 0)
    {
        LOG_ERROR("invalid buffer: p is null or n == 0");
        return;
    }


    size_t show = (n < max_show) ? n : max_show;
    for(size_t idx = 0; idx < show; idx++)
    {
        std::printf("%02X ", p[idx]);
    }
    if (show < n) std::printf("...");
}

void RtpInterleaved::bind(uint8_t ch, std::weak_ptr<RtpTrack> track, bool is_rtcp)
{
    map_.bind(ch, std::move(track), is_rtcp);
#if RTP_DEBUG
    LOG_INFO("Interleaved bind: ch=", (int)ch, " is_rtcp=", is_rtcp ? 1 : 0);
#endif
}

void RtpInterleaved::unbind(uint8_t ch)
{
    map_.unbind(ch);

#if RTP_DEBUG
    LOG_INFO("Interleaved unbind: ch=", (int)ch);
#endif
}

int RtpInterleaved::onInterleaved(uint8_t channel, const uint8_t *payload, size_t length)
{
#if RTP_DEBUG
    HexDumpPrefix(payload,length);
#endif



    
    return 0;
}