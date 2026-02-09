#ifndef _DEPACKETIZER_H__
#define _DEPACKETIZER_H__



#include <memory>
#include <vector>

class RtpPacket;  

class Depacketizer 
{
public:
    using Ptr = std::shared_ptr<RtpPacket>;
    virtual ~Depacketizer() = default;
    virtual bool input(const Ptr& pkt) = 0;
    virtual bool hasFrame() const = 0;
    virtual std::vector<uint8_t> popFrame() = 0;
};



#endif