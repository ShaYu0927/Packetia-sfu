#ifndef _MEDIAENDPOINT_H_
#define _MEDIAENDPOINT_H_

#include "EndpointBase.h"
#include "RtpReceiver.h"
#include "RtspMediaSession.h"

namespace media 
{

class MediaEndpoint : public utils::EndpointBase
{
public:
    using EndpointBase::EndpointBase;
    MediaEndpoint(uint64_t id,
                  std::shared_ptr<RtpTrack> tracker,
                  std::shared_ptr<MediaSession> session)
        : utils::EndpointBase(id, "MediaEndpoint"),
          tracker_(std::move(tracker)),
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

    std::shared_ptr<RtpTrack> SourceTrack() const
    {
        return tracker_;
    }

    std::shared_ptr<MediaSession> SourceSession() const
    {
        return session_;
    }

private:
    std::shared_ptr<MediaSession> session_;
    uint64_t id_;
    std::shared_ptr<RtpTrack> tracker_;
};

class SfuRouter
{
public:
    void AddSubscriber(uint32_t source_ssrc, std::shared_ptr<rtsp::RtpSenderTrack> sender);
    std::vector<std::shared_ptr<rtsp::RtpSenderTrack>> GetSenderTracks(uint32_t source_ssrc);

private:
    std::unordered_map<uint32_t, std::vector<std::shared_ptr<rtsp::RtpSenderTrack>>> subscribers_;
};

class SfuEndpoint : public MediaEndpoint
{
public:
    using MediaEndpoint::MediaEndpoint;

    bool Start() override;
    void Stop() override;

    void SetVideoTrack(const std::shared_ptr<VideoTrack>& track)
    {
        video_track_ = track;
    }

    bool InitTracks(const std::vector<TrackInfo>& infos);

protected:
    void HandleRtpPacket(Packet* pkt) override;
    void HandleRtcpPacket(Packet* pkt) override;

    void ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len);

private:
    rtsp::RtpReceiverTrack::Ptr FindTrackBySsrc(uint32_t ssrc);
    std::shared_ptr<rtsp::RtpReceiverTrack> GetOrCreateTrack(uint32_t ssrc);

private:
    std::mutex track_mtx_;
    std::unordered_map<uint32_t, rtsp::RtpReceiverTrack::Ptr> ssrc_to_track_;
    std::shared_ptr<VideoTrack> video_track_;

    SfuRouter router_;
};



}

#endif /* _MEDIAENDPOINT_H_ */
