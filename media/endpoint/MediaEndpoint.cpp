#include "MediaEndpoint.h"
#include "logger.h"
#include <mutex>

namespace media 
{

static bool TryGetRtpSsrc(const uint8_t* data, size_t len, uint32_t& ssrc)
{
    if (!data || len < 12)
    {
        return false;
    }

    const uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
    {
        return false;
    }

    ssrc = (static_cast<uint32_t>(data[8]) << 24) |
           (static_cast<uint32_t>(data[9]) << 16) |
           (static_cast<uint32_t>(data[10]) << 8) |
            static_cast<uint32_t>(data[11]);

    return true;
}

/* 网络协议包 转成 RtpPacket */
void MediaEndpoint::OnRtp(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleRtpPacket(pkt);
}

void MediaEndpoint::OnRtcp(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleRtcpPacket(pkt);
}

void MediaEndpoint::OnStun(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleStunPacket(pkt);
}

void MediaEndpoint::OnDtls(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleDtlsPacket(pkt);
}


RtpReceiverTrack::Ptr SfuEndpoint::FindTrackBySsrc(uint32_t ssrc)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    auto it = ssrc_to_track_.find(ssrc);
    if (it == ssrc_to_track_.end())
    {
        return nullptr;
    }

    return it->second;
}

void SfuEndpoint::HandleRtpPacket(Packet* pkt)
{
    if (!pkt || pkt->len <= 0)
    {
        LOG_ERROR("invalid packet");
        return;
    }

    uint32_t ssrc = 0;
    if (!TryGetRtpSsrc(pkt->data, pkt->len, ssrc))
    {
        LOG_ERROR("failed to get rtp ssrc, len=", pkt->len);
        return;
    }

    auto track = GetOrCreateTrack(ssrc);
    if (!track)
    {
        LOG_ERROR("get or create track failed, ssrc=", ssrc);
        return;
    }

    track->inputRtp(track->getTrackType(), 90000, pkt->data, pkt->len);
}

std::shared_ptr<RtpReceiverTrack> SfuEndpoint::GetOrCreateTrack(uint32_t ssrc)
{
    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        auto it = ssrc_to_track_.find(ssrc);
        if (it != ssrc_to_track_.end())
        {
            return it->second;
        }
    }

    TrackInfo info;
    info.ssrc = ssrc;
    info.type = TrackVideo;


    auto new_track = std::make_shared<RtpVideoTracker>(info);

    {
        std::lock_guard<std::mutex> lock(track_mtx_);
        auto it = ssrc_to_track_.find(ssrc);
        if (it != ssrc_to_track_.end())
        {
            return it->second;
        }

        ssrc_to_track_[ssrc] = new_track;
        return new_track;
    }
}

void SfuEndpoint::HandleRtcpPacket(Packet* pkt)
{
    if (!pkt)
    {
        LOG_ERROR("invalid packet");
        return;
    }

    if (!video_track_)
    {
        LOG_ERROR("video track not found");
        return;
    }
    video_track_->onInputRtcp(pkt->data,pkt->len);
}

bool SfuEndpoint::InitTracks(const std::vector<TrackInfo>& infos)
{
    for (const auto& info : infos)
    {
        if (info.type == TrackType::TrackVideo)
        {
            video_track_ = std::make_shared<VideoTrack>(info);
        }
    }
    return video_track_ != nullptr;
}

}