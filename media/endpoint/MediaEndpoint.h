#ifndef _MEDIAENDPOINT_H_
#define _MEDIAENDPOINT_H_

#include "EndpointBase.h"
#include "RtpReceiver.h"
#include "RtcpContext.h"
#include "core/EncodedFrameRouter.h"
#include "quality/WeakNetController.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <optional>

class MediaSession;
namespace sdp { struct SdpMedia; }

namespace media 
{

class MediaEndpoint : public utils::EndpointBase
{
public:
    using EndpointBase::EndpointBase;
    MediaEndpoint(uint64_t id,
                  std::shared_ptr<RtpTrackDescription> track_description,
                  std::shared_ptr<MediaSession> session);
    explicit MediaEndpoint(uint64_t id) : utils::EndpointBase(id, "MediaEndpoint") {}
    ~MediaEndpoint() = default;
    bool Start() override
    {
        SetState(State::kRunning);
        return true;
    }

    void Stop() override
    {
        SetState(State::kStopped);
    }

    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;
    void OnStun(WorkJob& job) override;
    void OnDtls(WorkJob& job) override;

protected:
    virtual void HandleRtpPacket(common::BufferView packet) = 0;
    virtual void HandleRtcpPacket(common::BufferView packet) {}
    virtual void HandleStunPacket(common::BufferView packet) {}
    virtual void HandleDtlsPacket(common::BufferView packet) {}

    std::string session_id_;
    std::string stream_id_;
};

class SfuEndpoint : public MediaEndpoint,
                    public std::enable_shared_from_this<SfuEndpoint>
{
public:
    SfuEndpoint(uint64_t id,
                std::shared_ptr<RtpTrackDescription> track_description,
                std::shared_ptr<MediaSession> session,
                std::shared_ptr<IEncodedFramePublisher> frame_publisher)
        : MediaEndpoint(id, track_description, std::move(session)),
          frame_publisher_(std::move(frame_publisher))
    {
        if (track_description)
            AddPublishedTrack(std::to_string(track_description->getTrackIndex()), track_description);
    }
    explicit SfuEndpoint(uint64_t id, std::shared_ptr<IEncodedFramePublisher> publisher = {})
        : MediaEndpoint(id), frame_publisher_(std::move(publisher)) {}
    ~SfuEndpoint() override { Stop(); }
    using SendRtcpCallback = std::function<bool(const uint8_t*, size_t)>;
    using EncodedFrameCallback = std::function<void(const media::EncodedFrame::ConstPtr&)>;
    using FrameSubscriptionId = uint64_t;

    // Control-plane operations are serialized with packet processing. A track
    // ID is stable; MID and SSRC are bindings inside this media session.
    bool AddPublishedTrack(const std::string& id, std::shared_ptr<RtpTrackDescription> description,
                           std::string mid = {}, uint8_t mid_extension_id = 0);
    bool AddPublishedTrack(const std::string& id, const sdp::SdpMedia& media, int track_index);
    // retire_identity=false is only for rolling back an unpublished SETUP.
    bool RemovePublishedTrack(const std::string& id, bool retire_identity = true);
    bool BindSsrc(const std::string& id, uint32_t ssrc);
    bool GetPublishedTrack(const std::string& id, ::TrackInfo& info) const;
    size_t PublishedTrackCount() const;
    size_t SubscriptionCount() const;
    void SetTrackRtcpSendCallback(const std::string& id, SendRtcpCallback cb);

    // Parameters must come from the subscriber's negotiation, never from the
    // publisher's PT/extmap. Configure before Room::SubscribeTrack.
    bool ConfigureSubscription(const std::string& id, const rtsp::RtpSenderTrackConfig& config,
                               std::shared_ptr<IMediaTransport> transport, CodecId codec);
    bool Subscribe(const std::string& id, const std::shared_ptr<SfuEndpoint>& source,
                   const std::string& source_track_id);
    bool Unsubscribe(const std::string& id);

    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;

    bool Start() override;
    void Stop() override;

    // Receive-stream removal retains the negotiated logical track.
    int RemoveMediaStream(uint32_t source_ssrc);

    void OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs);
    void OnTrackPli(uint32_t media_ssrc);
    void SetRtcpSendCallback(SendRtcpCallback cb);
    FrameSubscriptionId AddEncodedFrameCallback(EncodedFrameCallback cb);
    void RemoveEncodedFrameCallback(FrameSubscriptionId id);

protected:
    void HandleRtpPacket(common::BufferView packet) override;
    void HandleRtcpPacket(common::BufferView packet) override;

    void ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len);

private:
    struct Subscription;
    struct PublishedTrack
    {
        std::shared_ptr<RtpTrackDescription> description;
        std::string mid;
        uint8_t mid_extension_id = 0;
        SendRtcpCallback send_rtcp;
        std::vector<std::weak_ptr<Subscription>> subscribers;
    };
    struct Subscription
    {
        std::atomic<bool> active{true};
        std::weak_ptr<SfuEndpoint> source;
        std::weak_ptr<SfuEndpoint> destination;
        std::string source_track_id;
        std::optional<uint32_t> source_ssrc; // Selected encoding, protected by source endpoint.
        std::shared_ptr<rtsp::RtpSenderTrack> sender;
    };
    struct SubscriptionParameters
    {
        rtsp::RtpSenderTrackConfig config;
        std::shared_ptr<IMediaTransport> transport;
        CodecId codec = CodecId::Unknown;
    };
    bool ResolveRtpTrack(common::BufferView packet, const std::string& hint);
    SendRtcpCallback RtcpSenderFor(uint32_t ssrc);
    void Deliver(const std::shared_ptr<Subscription>& subscription, common::SharedBuffer packet);
    void RequestKeyFrame(const std::string& track_id);
    void PruneSubscriptions();
    mutable std::recursive_mutex runtime_mutex_;
    std::unordered_map<std::string, PublishedTrack> published_tracks_;
    std::unordered_map<uint32_t, std::string> ssrc_bindings_;
    // Removed identities cannot be learned again from late queued packets.
    std::unordered_set<uint32_t> retired_ssrcs_;
    std::unordered_set<std::string> retired_track_ids_;
    std::unordered_map<std::string, SubscriptionParameters> subscription_parameters_;
    std::unordered_map<std::string, std::shared_ptr<Subscription>> subscriptions_;
    rtsp::RtpReceiverTrack::Ptr FindReceiverTrackBySsrc(uint32_t ssrc);
    std::shared_ptr<rtsp::RtpReceiverTrack> GetOrCreateReceiverTrack(uint32_t ssrc);
    void RemoveMediaStreamOnOwner(uint32_t source_ssrc);
    void DispatchEncodedFrame(const media::EncodedFrame::Ptr& frame);
    void EvaluateReceiveQuality(uint32_t source_ssrc);

    struct ReceiveQualityState
    {
        WeakNetController controller;
        RtpRecvStatsBase::ReceiverReport latest_report;
        NetworkQualityLevel quality = NetworkQualityLevel::Unknown;
        uint64_t update_time_ms = 0;
    };

private:
    std::mutex track_mtx_;
    std::unordered_map<uint32_t, rtsp::RtpReceiverTrack::Ptr> ssrc_to_track_;
    std::vector<media::EncodedFrame::Ptr> pending_frames_;
    std::unique_ptr<rtsp::RtcpDispatcher> rtcp_dispatcher_;
    std::unique_ptr<rtcpx::IRtcpReceiver> rtcp_receiver_;
    std::mutex rtcp_send_mutex_;
    SendRtcpCallback send_rtcp_cb_;
    uint32_t local_rtcp_ssrc_ = 0;
    std::mutex frame_callbacks_mutex_;
    std::unordered_map<FrameSubscriptionId, EncodedFrameCallback> frame_callbacks_;
    std::atomic<FrameSubscriptionId> next_frame_subscription_id_{1};
    std::shared_ptr<IEncodedFramePublisher> frame_publisher_;
    std::atomic<uint64_t> published_frame_count_{0};
    std::mutex quality_mutex_;
    std::unordered_map<uint32_t, ReceiveQualityState> receive_quality_;
};



}

#endif /* _MEDIAENDPOINT_H_ */
