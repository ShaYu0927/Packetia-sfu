#ifndef PACKETIA_WEBRTC_MEDIA_TRANSPORT_H
#define PACKETIA_WEBRTC_MEDIA_TRANSPORT_H

#include "IMediaTransport.h"
#include "WebRtcSession.h"
#include "SendSideController.h"
#include "PacedSendQueue.h"
#include <functional>
#include <mutex>
#include <unordered_set>

namespace media::transport
{
// Construct this adapter first, pass it as WebRtcSession's packet sink, then
// BindSession and SetPacketSink(MediaEndpointIngress). Session ownership stays
// with the application. All session operations must use its event loop.
// Connect options.onNetworkPathChanged to OnNetworkPathChanged before starting.
class WebRtcMediaTransport final : public IMediaTransport, public IMediaPacketSink,
                                  public std::enable_shared_from_this<WebRtcMediaTransport>
{
public:
    using Dispatch = std::function<bool(std::function<void()>)>;
    WebRtcMediaTransport(uint64_t id, Dispatch dispatch, PacedSendQueue::Schedule schedule);
    bool BindSession(const std::shared_ptr<protocol::webrtc::WebRtcSession>& session);
    uint64_t Id() const noexcept override { return id_; }
    MediaTransportProtocol Protocol() const noexcept override { return MediaTransportProtocol::WebRtc; }
    MediaTransportState State() const noexcept override;
    // Ok means accepted by the bounded pacer; Full maps to NotWritable.
    // Encryption and final network dispatch remain on the session owner loop.
    SendResult Send(MediaPacketType type, const uint8_t* data, size_t size, bool retransmit = false) override;
    SendResult SendRtpWithCompletion(const uint8_t* data, size_t size, bool retransmit,
                                     std::function<void()> on_sent) override;
    SendResult SendWithValidity(MediaPacketType type, const uint8_t* data, size_t size, bool retransmit,
                               std::function<bool()> valid, std::function<void()> on_sent = {}) override;
    void Close() override;
    void SetPacketSink(std::weak_ptr<IMediaPacketSink> sink) override;
    MediaPacketIngressResult OnMediaPacket(ReceivedMediaPacket packet) override;
    std::shared_ptr<media::SendSideController> GetSendSideController() override { return controller_; }
    // Wire WebRtcSessionOptions::onNetworkPathChanged on the session owner loop.
    void OnNetworkPathChanged(const protocol::webrtc::WebRtcNetworkPath& path);
    PacedSendQueue::Stats GetPacerStats() const;

private:
    SendResult SendWithCompletion(MediaPacketType type, const uint8_t* data, size_t size,
                                  std::function<void()> on_sent, std::function<bool()> valid = {});
    const uint64_t id_;
    const Dispatch dispatch_;
    const PacedSendQueue::Schedule schedule_;
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> path_epoch_{0};
    mutable std::mutex mutex_;
    std::weak_ptr<protocol::webrtc::WebRtcSession> session_;
    std::weak_ptr<IMediaPacketSink> sink_;
    std::shared_ptr<PacedSendQueue> pacer_;
    std::unordered_set<uint32_t> audio_ssrcs_;
    const std::shared_ptr<media::SendSideController> controller_;
};
}
#endif
