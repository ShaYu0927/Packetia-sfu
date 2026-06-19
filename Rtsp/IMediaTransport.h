#ifndef _I_MEDIA_TRANSPORT_H_
#define _I_MEDIA_TRANSPORT_H_

#include <cstddef>
#include <cstdint>

enum class PacketType
{
    Unknown = 0,
    Rtp,
    Rtcp
};

enum class SendResult
{
    Ok = 0,
    Failed,
    Closed,
    NotWritable
};

struct SendOptions
{
    PacketType type = PacketType::Unknown;
    
    uint16_t seq = 0;
    uint32_t ssrc = 0;

    bool retransmit = false;
    bool allow_queue = true;
};

class IPacketSender
{
public:
    virtual ~IPacketSender() = default;

    virtual bool IsClosed() const
    {
        return false;
    }

    virtual bool IsWritable() const
    {
        return true;
    }

    virtual SendResult SendPacket(const uint8_t* data,
                                  size_t size,
                                  const SendOptions& options)
    {
        (void)data;
        (void)size;
        (void)options;
        return SendResult::Failed;
    }
};


#endif /* _I_MEDIA_TRANSPORT_H_ */