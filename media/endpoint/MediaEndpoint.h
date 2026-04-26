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
    bool Start() override { return true;}

    void Stop() override { }

    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;
    void OnStun(WorkJob& job) override;
    void OnDtls(WorkJob& job) override;

protected:
    virtual void HandleRtpPacket(Packet* pkt) = 0;
    virtual void HandleRtcpPacket(Packet* pkt) {}
    virtual void HandleStunPacket(Packet* pkt) {}
    virtual void HandleDtlsPacket(Packet* pkt) {}

    

private:
    std::shared_ptr<MediaSession> session_;
    uint64_t id_;
    std::shared_ptr<RtpTrack> tracker_;
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

private:
    RtpReceiverTrack::Ptr FindTrackBySsrc(uint32_t ssrc);
    std::shared_ptr<RtpReceiverTrack> GetOrCreateTrack(uint32_t ssrc);

private:
    std::mutex track_mtx_;
    std::unordered_map<uint32_t, RtpReceiverTrack::Ptr> ssrc_to_track_;
    std::shared_ptr<VideoTrack> video_track_;
};



}

#endif /* _MEDIAENDPOINT_H_ */