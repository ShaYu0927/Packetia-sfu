#ifndef PACKETIA_MEDIA_SFU_ENDPOINT_H_
#define PACKETIA_MEDIA_SFU_ENDPOINT_H_

#include "EndpointBase.h"
#include "RtpReceiver.h"
#include "RtcpContext.h"
#include "EncodedFrameRouter.h"
#include "WeakNetController.h"
#include "KeyFrameRequestLimiter.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <optional>
#include <vector>

class MediaSession;
namespace sdp { struct SdpMedia; }

namespace media 
{

class SfuEndpoint : public utils::EndpointBase,
                    public std::enable_shared_from_this<SfuEndpoint>
{
public:
    SfuEndpoint(uint64_t id,
                std::shared_ptr<RtpTrackDescription> track_description,
                std::shared_ptr<MediaSession> session,
                std::shared_ptr<IEncodedFramePublisher> frame_publisher);
    explicit SfuEndpoint(uint64_t id, std::shared_ptr<IEncodedFramePublisher> publisher = {})
        : utils::EndpointBase(id, "SfuEndpoint"), frame_publisher_(std::move(publisher)) {}
    ~SfuEndpoint() override { Stop(); }
    using SendRtcpCallback = std::function<bool(const uint8_t*, size_t)>;
    using EncodedFrameCallback = std::function<void(const media::EncodedFrame::ConstPtr&)>;
    using FrameSubscriptionId = uint64_t;

    // Control-plane operations are serialized with packet processing. A track
    // ID is stable; MID and SSRC are bindings inside this media session.
    bool AddPublishedTrack(const std::string& id, std::shared_ptr<RtpTrackDescription> description,
                           std::string mid = {}, uint8_t mid_extension_id = 0);
    bool AddPublishedTrack(const std::string& id, const sdp::SdpMedia& media, int track_index);
    // Replace the complete receive set atomically, using stable nonempty MIDs
    // and one supported primary codec per media. Empty pauses every track.
    // Unchanged receivers, track indices and compatible subscriptions survive;
    // changing codec/rate/channels/fmtp with live subscribers is rejected.
    // Paused tracks detach subscribers but retain MID/index/RTCP metadata for
    // resume. Old SSRCs cannot move to another MID in this endpoint.
    bool ReplacePublishedTracks(const std::vector<sdp::SdpMedia>& medias);
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
    bool PollMedia(uint64_t stream_key, uint64_t now_ms) override;

    // Receive-stream removal retains the negotiated logical track.
    int RemoveMediaStream(uint32_t source_ssrc);

    void OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs);
    void OnTrackPli(uint32_t media_ssrc);
    // Run on the media pool; also drives loss recovery while RTP is silent.
    void TickRecovery();
    bool ScheduleRecoveryTick();
    void SetRtcpSendCallback(SendRtcpCallback cb);
    FrameSubscriptionId AddEncodedFrameCallback(EncodedFrameCallback cb);
    void RemoveEncodedFrameCallback(FrameSubscriptionId id);

protected:
    void HandleRtpPacket(common::BufferView packet);
    void HandleRtcpPacket(common::BufferView packet);

    void ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len);

    // Source identity attached to encoded-frame events by RTSP/WebRTC owners.
    std::string session_id_;
    std::string stream_id_;

private:
    struct Subscription;
    struct PublishedTrack
    {
        std::shared_ptr<RtpTrackDescription> description;
        std::string mid;
        uint8_t mid_extension_id = 0;
        SendRtcpCallback send_rtcp;
        std::vector<std::weak_ptr<Subscription>> subscribers;
        KeyFrameRequestLimiter keyframe_requests;
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
    struct PausedPublishedTrack
    {
        PublishedTrack track;
        std::vector<uint32_t> ssrcs;
    };
    bool ResolveRtpTrack(common::BufferView packet, const std::string& hint);
    SendRtcpCallback RtcpSenderFor(uint32_t ssrc);
    void Deliver(const std::shared_ptr<Subscription>& subscription, common::SharedBuffer packet);
    void RequestKeyFrame(const std::string& track_id);
    void PruneSubscriptions();
    mutable std::recursive_mutex runtime_mutex_;
    std::unordered_map<std::string, PublishedTrack> published_tracks_;
    std::unordered_map<std::string, PausedPublishedTrack> paused_published_tracks_;
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
    std::unordered_map<uint64_t, std::weak_ptr<rtsp::RtpReceiverTrack>> receiver_tick_keys_;
    std::vector<media::EncodedFrame::Ptr> pending_frames_;
    std::unique_ptr<rtsp::RtcpDispatcher> rtcp_dispatcher_;
    std::unique_ptr<rtcpx::IRtcpReceiver> rtcp_receiver_;
    std::mutex rtcp_send_mutex_;
    SendRtcpCallback send_rtcp_cb_;
    uint32_t local_rtcp_ssrc_ = 0;
    KeyFrameRequestLimiter unbound_keyframe_requests_;
    std::mutex frame_callbacks_mutex_;
    std::unordered_map<FrameSubscriptionId, EncodedFrameCallback> frame_callbacks_;
    std::atomic<FrameSubscriptionId> next_frame_subscription_id_{1};
    std::shared_ptr<IEncodedFramePublisher> frame_publisher_;
    std::atomic<uint64_t> published_frame_count_{0};
    std::mutex quality_mutex_;
    std::unordered_map<uint32_t, ReceiveQualityState> receive_quality_;
};



}

#endif // PACKETIA_MEDIA_SFU_ENDPOINT_H_
