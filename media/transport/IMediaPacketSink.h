#ifndef _I_MEDIA_PACKET_SINK_H_
#define _I_MEDIA_PACKET_SINK_H_

#include "MediaTransportPacket.h"

#include <memory>

enum class MediaPacketIngressResult
{
    Accepted = 0,
    Dropped,
    Closed
};

/*
 * Consumer boundary for complete RTP/RTCP packets.
 *
 * The packet is passed by value so ownership can be transferred to an
 * asynchronous media worker with std::move and without retaining an I/O
 * buffer pointer.
 */
class IMediaPacketSink
{
public:
    virtual ~IMediaPacketSink() = default;

    virtual MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) = 0;
};

/*
 * Producer boundary implemented by a receiving transport.
 *
 * A weak sink reference prevents the transport/session/ingress ownership
 * cycle. Passing an empty weak_ptr detaches the receive side during shutdown.
 */
class IMediaPacketSource
{
public:
    virtual ~IMediaPacketSource() = default;

    virtual void SetPacketSink(std::weak_ptr<IMediaPacketSink> sink) = 0;
};

#endif /* _I_MEDIA_PACKET_SINK_H_ */
