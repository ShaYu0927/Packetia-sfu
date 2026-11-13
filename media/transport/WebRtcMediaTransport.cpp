#include "WebRtcMediaTransport.h"

namespace media::transport
{
WebRtcMediaTransport::WebRtcMediaTransport(uint64_t id, Dispatch dispatch, PacedSendQueue::Schedule schedule)
    : id_(id), dispatch_(std::move(dispatch)), schedule_(std::move(schedule)),
      controller_(std::make_shared<media::SendSideController>())
{
    controller_->SetNetworkAvailable(false);
}

bool WebRtcMediaTransport::BindSession(const std::shared_ptr<protocol::webrtc::WebRtcSession>& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto weak = weak_from_this();
    if (closed_ || !dispatch_ || !schedule_ || weak.expired() || !id_ || !session ||
        session->TransportId() != id_ || !session_.expired()) return false;
    for (const auto& media : session->LocalDescription().medias)
        if (media.port && media.media == "audio" &&
            (media.direction == sdp::MediaDirection::SendOnly || media.direction == sdp::MediaDirection::SendRecv))
            for (const auto& source : media.ssrcs) audio_ssrcs_.insert(source.ssrc);
    pacer_ = std::make_shared<PacedSendQueue>(schedule_, [weak](PacedSendQueue::Packet packet) {
        const auto adapter = weak.lock();
        if (!adapter || adapter->closed_) return;
        std::shared_ptr<protocol::webrtc::WebRtcSession> session;
        {
            std::lock_guard<std::mutex> lock(adapter->mutex_);
            session = adapter->session_.lock();
        }
        if (!session) return;
        if (packet.priority == PacedSendQueue::Priority::Control)
            session->SendRtcp(std::move(packet.bytes));
        else session->SendRtpWithCompletion(std::move(packet.bytes), std::move(packet.valid), std::move(packet.on_sent));
    }, [controller = controller_] {
        const auto budget = controller->GetBandwidthBudget();
        if (!budget.network_available) return uint32_t{0};
        const auto update = controller->GetNetworkState();
        return update.has_pacer_config ? update.pacer_config.pacing_bitrate_bps : budget.start_bps;
    });
    session_ = session;
    return true;
}

void WebRtcMediaTransport::OnNetworkPathChanged(const protocol::webrtc::WebRtcNetworkPath& path)
{
    if (closed_) return;
    path_epoch_.fetch_add(1, std::memory_order_acq_rel);
    std::shared_ptr<PacedSendQueue> pacer;
    { std::lock_guard<std::mutex> lock(mutex_); pacer = pacer_; }
    if (pacer) pacer->Reset();
    if (path.state == protocol::webrtc::WebRtcNetworkPathState::Ready)
        controller_->OnNetworkRouteChanged();
    else
        controller_->SetNetworkAvailable(false);
    // Extension point: coalesce a video key-frame request on recovered paths.
    // Keep track lookup and worker dispatch in the endpoint/application layer.
}

MediaTransportState WebRtcMediaTransport::State() const noexcept
{
    if (closed_) return MediaTransportState::Closed;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = session_.lock();
    if (!session) return MediaTransportState::Created;
    using State = protocol::webrtc::WebRtcSessionState;
    switch (session->State())
    {
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
    return SendWithCompletion(type, data, size, {});
}

SendResult WebRtcMediaTransport::SendRtpWithCompletion(const uint8_t* data, size_t size, bool,
                                                        std::function<void()> on_sent)
{
    return SendWithCompletion(MediaPacketType::Rtp, data, size, std::move(on_sent));
}

SendResult WebRtcMediaTransport::SendWithCompletion(MediaPacketType type, const uint8_t* data,
                                                     size_t size, std::function<void()> on_sent,
                                                     std::function<bool()> valid)
{
    const auto epoch = path_epoch_.load(std::memory_order_acquire);
    if (IsClosed()) return SendResult::Closed;
    if (!IsWritable()) return SendResult::NotWritable;
    if (!data || !size || size > 65507 || (type != MediaPacketType::Rtp && type != MediaPacketType::Rtcp)) return SendResult::Failed;
    if (type == MediaPacketType::Rtp && (size < 12 || (data[0] >> 6) != 2)) return SendResult::Failed;
    const auto weak = weak_from_this();
    if (weak.expired()) return SendResult::Failed;
    std::shared_ptr<PacedSendQueue> pacer;
    auto priority = PacedSendQueue::Priority::Control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pacer = pacer_;
        if (type == MediaPacketType::Rtp)
        {
            const auto ssrc = (uint32_t{data[8]} << 24) | (uint32_t{data[9]} << 16) |
                (uint32_t{data[10]} << 8) | data[11];
            priority = audio_ssrcs_.count(ssrc) ? PacedSendQueue::Priority::Audio : PacedSendQueue::Priority::Media;
        }
    }
    if (!pacer) return SendResult::NotWritable;
    const auto result = pacer->Send(priority, data, size, [weak, epoch, valid = std::move(valid)] {
        const auto current = weak.lock();
        return current && !current->closed_ && current->path_epoch_.load(std::memory_order_acquire) == epoch &&
            (!valid || valid());
    }, std::move(on_sent));
    switch (result)
    {
        case PacedSendQueue::Result::Accepted: return SendResult::Ok;
        case PacedSendQueue::Result::Full: return SendResult::NotWritable;
        default: return SendResult::Closed;
    }
}

SendResult WebRtcMediaTransport::SendWithValidity(MediaPacketType type, const uint8_t* data, size_t size,
                                                 bool, std::function<bool()> valid,
                                                 std::function<void()> on_sent)
{
    if (valid && !valid()) return SendResult::NotWritable;
    return SendWithCompletion(type, data, size, std::move(on_sent), std::move(valid));
}

PacedSendQueue::Stats WebRtcMediaTransport::GetPacerStats() const
{
    std::shared_ptr<PacedSendQueue> pacer;
    { std::lock_guard<std::mutex> lock(mutex_); pacer = pacer_; }
    return pacer ? pacer->GetStats() : PacedSendQueue::Stats{};
}

MediaPacketIngressResult WebRtcMediaTransport::OnMediaPacket(ReceivedMediaPacket packet)
{
    if (IsClosed()) return MediaPacketIngressResult::Closed;
    if (!IsWritable() || !packet.IsValid() || packet.transport_id != id_) return MediaPacketIngressResult::Dropped;
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
    std::shared_ptr<PacedSendQueue> pacer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_.reset();
        session = session_;
        pacer = pacer_;
    }
    if (pacer) pacer->Close();
    if (dispatch_) dispatch_([session] { if (auto owner = session.lock()) owner->stop(); });
}
}
