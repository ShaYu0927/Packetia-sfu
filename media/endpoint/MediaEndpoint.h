#ifndef _MEDIAENDPOINT_H_
#define _MEDIAENDPOINT_H_

#include "EndpointBase.h"
#include "RtpReceiver.h"
#include "RtspMediaSession.h"
#include "RtcpContext.h"
#include "core/EncodedFrameRouter.h"
#include "quality/WeakNetController.h"
#include <functional>
#include <atomic>
#include <mutex>

namespace media 
{

class MediaEndpoint : public utils::EndpointBase
{
public:
    using EndpointBase::EndpointBase;
    MediaEndpoint(uint64_t id,
                  std::shared_ptr<RtpTrackDescription> track_description,
                  std::shared_ptr<MediaSession> session)
        : utils::EndpointBase(id, "MediaEndpoint"),
          track_description_(std::move(track_description)),
          session_(std::move(session))
    {
    }
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
    virtual void HandleRtpPacket(Packet* pkt) = 0;
    virtual void HandleRtcpPacket(Packet* pkt) {}
    virtual void HandleStunPacket(Packet* pkt) {}
    virtual void HandleDtlsPacket(Packet* pkt) {}

    std::shared_ptr<RtpTrackDescription> SourceTrack() const
    {
        return track_description_;
    }

    std::shared_ptr<MediaSession> SourceSession() const
    {
        return session_;
    }

private:
    std::shared_ptr<MediaSession> session_;
    uint64_t id_;
    std::shared_ptr<RtpTrackDescription> track_description_;
};

class SfuRouter
{
public:
    void AddSubscriber(uint32_t source_ssrc, std::shared_ptr<rtsp::RtpSenderTrack> sender);
    void RemoveSubscriber(uint32_t source_ssrc, const rtsp::RtpSenderTrack* sender);
    void RemoveStream(uint32_t source_ssrc);
    std::vector<std::shared_ptr<rtsp::RtpSenderTrack>> GetSenderTracks(uint32_t source_ssrc);

private:
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::vector<std::shared_ptr<rtsp::RtpSenderTrack>>> subscribers_;
};

class SfuEndpoint : public MediaEndpoint,
                    public std::enable_shared_from_this<SfuEndpoint>
{
public:
    SfuEndpoint(uint64_t id,
                std::shared_ptr<RtpTrackDescription> track_description,
                std::shared_ptr<MediaSession> session,
                std::shared_ptr<IEncodedFramePublisher> frame_publisher)
        : MediaEndpoint(id, std::move(track_description), std::move(session)),
          frame_publisher_(std::move(frame_publisher))
    {
    }
    using SendRtcpCallback = std::function<bool(const uint8_t*, size_t)>;
    using EncodedFrameCallback = std::function<void(const media::EncodedFrame::ConstPtr&)>;
    using FrameSubscriptionId = uint64_t;

    bool Start() override;
    void Stop() override;

    // Stream mutations are asynchronous and execute on the same media worker
    // as RTP/RTCP for source_ssrc.
    int AddSubscriber(uint32_t source_ssrc,
                      std::shared_ptr<rtsp::RtpSenderTrack> sender);
    int RemoveSubscriber(uint32_t source_ssrc,
                         const std::shared_ptr<rtsp::RtpSenderTrack>& sender);
    int RemoveMediaStream(uint32_t source_ssrc);

    void OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs);
    void OnTrackPli(uint32_t media_ssrc);
    void SetRtcpSendCallback(SendRtcpCallback cb);
    FrameSubscriptionId AddEncodedFrameCallback(EncodedFrameCallback cb);
    void RemoveEncodedFrameCallback(FrameSubscriptionId id);

protected:
    void HandleRtpPacket(Packet* pkt) override;
    void HandleRtcpPacket(Packet* pkt) override;

    void ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len);

private:
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
    SfuRouter router_;
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
