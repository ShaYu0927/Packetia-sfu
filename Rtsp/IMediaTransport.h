#ifndef _I_MEDIA_TRANSPORT_H_
#define _I_MEDIA_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include "IMediaPacketSink.h"

namespace media { class SendSideController; }

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
    Udp,
    WebRtc
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

    // The default preserves Send's success semantics, which may mean enqueue.
    // Implementations with a later completion boundary should override this.
    // The callback can run synchronously or on the transport's owner thread.
    virtual SendResult SendRtpWithCompletion(const uint8_t* data, size_t size,
                                             bool retransmit, std::function<void()> on_sent)
    {
        const auto result = Send(MediaPacketType::Rtp, data, size, retransmit);
        if (result == SendResult::Ok && on_sent) on_sent();
        return result;
    }

    // The default checks admission only; paced transports carry the predicate
    // through final dispatch. It must be thread-safe and not borrow packet data.
    virtual SendResult SendWithValidity(MediaPacketType type, const uint8_t* data, size_t size,
                                       bool retransmit, std::function<bool()> valid,
                                       std::function<void()> on_sent = {})
    {
        if (valid && !valid()) return SendResult::NotWritable;
        return type == MediaPacketType::Rtp ?
            SendRtpWithCompletion(data, size, retransmit, std::move(on_sent)) : Send(type, data, size, retransmit);
    }

    virtual void Close() = 0;

    // Optional transport-wide sending state. Implementations without feedback
    // support can retain the default; UDP/interleaved transports share one
    // controller across every sender track using that transport.
    virtual std::shared_ptr<media::SendSideController> GetSendSideController()
    { return nullptr; }

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
