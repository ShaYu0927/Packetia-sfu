#include "MediaEndpoint.h"
#include "logger.h"
#include <iomanip>
#include <mutex>

namespace media 
{

static void DumpBytes(const uint8_t* data, size_t len, size_t max_dump = 16)
{
    if (!data)
        return;

    std::ostringstream oss;
    size_t n = std::min(len, max_dump);
    for (size_t i = 0; i < n; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    LOG_INFO("packet dump len=", len, " bytes=", oss.str());
}

static bool TryGetRtpSsrc(const uint8_t* data, size_t len, uint32_t& ssrc)
{
    const uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
    {
        return false;
    }

    ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) | (static_cast<uint32_t>(data[10]) << 8) | static_cast<uint32_t>(data[11]);
    return true;
}

/* 网络协议包 转成 RtpPacket */
void MediaEndpoint::OnRtp(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        LOG_ERROR("invalid raw rtp packet");
        return;
    }
    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleRtpPacket(&pkt);
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
    if (!pkt || pkt->len < 12)
    {
        LOG_ERROR("[RTP] invalid packet: pkt=%p len=%u", pkt, pkt ? pkt->len : 0);
        return;
    }

    const uint8_t* data = pkt->data;

    uint8_t vpxcc = data[0];
    uint8_t mpt   = data[1];

    uint8_t version = (vpxcc >> 6) & 0x03;
    uint8_t padding = (vpxcc >> 5) & 0x01;
    uint8_t extension = (vpxcc >> 4) & 0x01;
    uint8_t csrc_count = vpxcc & 0x0F;

    uint8_t marker = (mpt >> 7) & 0x01;
    uint8_t payload_type = mpt & 0x7F;

    uint16_t seq = (data[2] << 8) | data[3];

    uint32_t timestamp =
        (uint32_t(data[4]) << 24) |
        (uint32_t(data[5]) << 16) |
        (uint32_t(data[6]) << 8)  |
        data[7];

    uint32_t ssrc =
        (uint32_t(data[8]) << 24) |
        (uint32_t(data[9]) << 16) |
        (uint32_t(data[10]) << 8) |
        data[11];

    TryGetRtpSsrc(pkt->data, pkt->len, ssrc);

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
            LOG_DEBUG("[TRACK] found existing track", "ssrc=", ssrc, "track_ptr=", it->second.get());
            return it->second;
        }
    }
    LOG_INFO("[TRACK] track not found, creating new track", "ssrc=", ssrc);

    TrackInfo info;
    info.ssrc = ssrc;
    info.type = TrackVideo;

    auto new_track = std::make_shared<RtpVideoTracker>(info);

    LOG_INFO("[TRACK] create new track (pre-insert)", "ssrc=", ssrc, "track_ptr=", new_track.get());

    {
        std::lock_guard<std::mutex> lock(track_mtx_);

        auto it = ssrc_to_track_.find(ssrc);
        if (it != ssrc_to_track_.end())
        {
            LOG_ERROR("[TRACK] race detected, track already created by another thread",
                     "ssrc=", ssrc,
                     "exist_ptr=", it->second.get(),
                     "new_ptr=", new_track.get());
            return it->second;
        }

        ssrc_to_track_[ssrc] = new_track;

        LOG_INFO("[TRACK] insert new track", "ssrc=", ssrc, "track_ptr=", new_track.get(), "total_tracks=", ssrc_to_track_.size());

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


bool SfuEndpoint::Start()
{
    return true;
}

void SfuEndpoint::Stop()
{
    std::lock_guard<std::mutex> lock(track_mtx_);
    ssrc_to_track_.clear();
}
}