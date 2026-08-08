#ifndef _I_MEDIA_TRANSPORT_H_
#define _I_MEDIA_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include "IMediaPacketSink.h"

enum class SendResult
{
    Ok = 0,
    Failed,
    Closed,
    NotWritable
};

enum class MediaTransportProtocol
{
    RtspInterleaved = 0,
    Udp
};

enum class MediaTransportState
{
    Created = 0,
    Connecting,
    Connected,
    Closing,
    Closed,
    Failed
};

/*
 * Protocol-neutral RTP/RTCP byte transport.
 *
 * Implementations own protocol framing, peer addressing and I/O-thread
 * dispatch. RTP/RTCP parsing, packet rewriting, retransmission and room/track
 * routing stay outside this interface.
 */
class IMediaTransport : public IMediaPacketSource
{
public:
    virtual ~IMediaTransport() = default;

    virtual uint64_t Id() const noexcept = 0;
    virtual MediaTransportProtocol Protocol() const noexcept = 0;
    virtual MediaTransportState State() const noexcept = 0;

    virtual SendResult Send(MediaPacketType type,
                            const uint8_t* data,
                            size_t size,
                            bool retransmit = false) = 0;

    virtual void Close() = 0;

    bool IsClosed() const noexcept
    {
        const auto state = State();
        return state == MediaTransportState::Closed ||
               state == MediaTransportState::Failed;
    }

    bool IsWritable() const noexcept
    {
        return State() == MediaTransportState::Connected;
    }

    SendResult SendRtp(const uint8_t* data,
                       size_t size,
                       bool retransmit = false)
    {
        return Send(MediaPacketType::Rtp, data, size, retransmit);
    }

    SendResult SendRtcp(const uint8_t* data, size_t size)
    {
        return Send(MediaPacketType::Rtcp, data, size, false);
    }
};

#endif /* _I_MEDIA_TRANSPORT_H_ */
