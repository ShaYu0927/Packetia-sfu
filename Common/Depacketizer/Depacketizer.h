#ifndef _DEPACKETIZER_H__
#define _DEPACKETIZER_H__



#include <memory>
#include <vector>

struct RtpView 
{
    uint16_t seq = 0;
    uint32_t ts = 0;
    bool marker = false;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    uint32_t ssrc = 0;
    bool valid() const { return payload && payload_len; }
};

struct iVideoFrame
{
    uint32_t ssrc = 0;
    uint32_t ts = 0;
    std::vector<uint8_t> annexb;
};

class Depacketizer 
{
public:
    virtual ~Depacketizer() = default;
    virtual bool input(const RtpView& pkt) = 0;
    virtual bool hasFrame() const = 0;
    virtual std::vector<uint8_t> popFrame() = 0;
};



#endif