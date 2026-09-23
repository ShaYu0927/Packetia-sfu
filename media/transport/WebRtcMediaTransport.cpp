#include "WebRtcMediaTransport.h"

namespace media::transport
{
WebRtcMediaTransport::WebRtcMediaTransport(uint64_t id, Dispatch dispatch)
    : id_(id), dispatch_(std::move(dispatch)), controller_(std::make_shared<media::SendSideController>())
{
    controller_->SetNetworkAvailable(false);
}

bool WebRtcMediaTransport::BindSession(const std::shared_ptr<protocol::webrtc::WebRtcSession>& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !dispatch_ || !id_ || !session || session->TransportId() != id_ || !session_.expired()) return false;
    session_ = session;
    return true;
}

MediaTransportState WebRtcMediaTransport::State() const noexcept
{
    if (closed_) return MediaTransportState::Closed;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = session_.lock();
    if (!session) return MediaTransportState::Created;
    using State = protocol::webrtc::WebRtcSessionState;
    switch (session->State()) {
    case State::New: return MediaTransportState::Created;
    case State::Connected: return MediaTransportState::Connected;
    case State::Closed: return MediaTransportState::Closed;
    case State::Failed: return MediaTransportState::Failed;
    default: return MediaTransportState::Connecting;
    }
}

void WebRtcMediaTransport::SetPacketSink(std::weak_ptr<IMediaPacketSink> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

SendResult WebRtcMediaTransport::Send(MediaPacketType type, const uint8_t* data, size_t size, bool)
{
    if (IsClosed()) return SendResult::Closed;
    if (!IsWritable()) return SendResult::NotWritable;
    if (!data || !size || (type != MediaPacketType::Rtp && type != MediaPacketType::Rtcp)) return SendResult::Failed;
    std::vector<uint8_t> packet(data, data + size);
    const auto weak = weak_from_this();
    if (weak.expired()) return SendResult::Failed;
    controller_->SetNetworkAvailable(true);
    return dispatch_([weak, type, packet = std::move(packet)]() mutable {
        const auto adapter = weak.lock();
        if (!adapter || adapter->closed_) return;
        std::shared_ptr<protocol::webrtc::WebRtcSession> session;
        {
            std::lock_guard<std::mutex> lock(adapter->mutex_);
            session = adapter->session_.lock();
        }
        if (!session) return;
        if (type == MediaPacketType::Rtp) session->SendRtp(std::move(packet));
        else session->SendRtcp(std::move(packet));
    }) ? SendResult::Ok : SendResult::Failed;
}

MediaPacketIngressResult WebRtcMediaTransport::OnMediaPacket(ReceivedMediaPacket packet)
{
    if (IsClosed()) return MediaPacketIngressResult::Closed;
    if (!IsWritable() || !packet.IsValid() || packet.transport_id != id_) return MediaPacketIngressResult::Dropped;
    controller_->SetNetworkAvailable(true);
    if (packet.type == MediaPacketType::Rtcp &&
        !controller_->OnRtcpPacket(packet.Data(), packet.Size(), packet.receive_time_ms))
        return MediaPacketIngressResult::Dropped;
    std::shared_ptr<IMediaPacketSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_.lock();
    }
    return sink ? sink->OnMediaPacket(std::move(packet)) : MediaPacketIngressResult::Closed;
}

void WebRtcMediaTransport::Close()
{
    if (closed_.exchange(true)) return;
    controller_->SetNetworkAvailable(false);
    std::weak_ptr<protocol::webrtc::WebRtcSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_.reset();
        session = session_;
    }
    if (dispatch_) dispatch_([session] { if (auto owner = session.lock()) owner->stop(); });
}
}
