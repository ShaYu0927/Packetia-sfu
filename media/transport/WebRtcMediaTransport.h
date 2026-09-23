#ifndef PACKETIA_WEBRTC_MEDIA_TRANSPORT_H
#define PACKETIA_WEBRTC_MEDIA_TRANSPORT_H

#include "IMediaTransport.h"
#include "protocol/WebRtc/WebRtcSession.h"
#include "quality/SendSideController.h"
#include <functional>
#include <mutex>

namespace media::transport
{
// Construct this adapter first, pass it as WebRtcSession's packet sink, then
// BindSession and SetPacketSink(MediaEndpointIngress). Session ownership stays
// with the application. All session operations must use its event loop.
class WebRtcMediaTransport final : public IMediaTransport, public IMediaPacketSink,
                                  public std::enable_shared_from_this<WebRtcMediaTransport>
{
public:
    using Dispatch = std::function<bool(std::function<void()>)>;
    WebRtcMediaTransport(uint64_t id, Dispatch dispatch);
    bool BindSession(const std::shared_ptr<protocol::webrtc::WebRtcSession>& session);
    uint64_t Id() const noexcept override { return id_; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::WebRtc; }
    MediaTransportState State() const noexcept override;
    // Ok means accepted by the event-loop queue, matching other asynchronous
    // transports. Encryption and the final network write occur on that loop.
    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size, bool retransmit = false) override;
    void Close() override;
    void SetPacketSink(std::weak_ptr<IMediaPacketSink> sink) override;
    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override;
    std::shared_ptr<media::SendSideController> GetSendSideController() override { return controller_; }

private:
    const uint64_t id_;
    const Dispatch dispatch_;
    std::atomic<bool> closed_{false};
    mutable std::mutex mutex_;
    std::weak_ptr<protocol::webrtc::WebRtcSession> session_;
    std::weak_ptr<IMediaPacketSink> sink_;
    const std::shared_ptr<media::SendSideController> controller_;
};
}
#endif
