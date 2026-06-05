#include "MediaEndpoint.h"
#include "logger.h"
#include "RtcpContext.h"
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
    if (!data || len < 12)
    {
        return false;
    }

    const uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2)
    {
        return false;
    }

    ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) | (static_cast<uint32_t>(data[10]) << 8) | static_cast<uint32_t>(data[11]);
    return true;
}

static uint16_t ReadUint16BE(const uint8_t* data)
{
    return (static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]);
}

static uint32_t ReadUint32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

static bool TryGetRtcpMediaSsrc(const uint8_t* data, size_t len, uint32_t& ssrc)
{
    if (!data || len < 4)
    {
        return false;
    }

    size_t offset = 0;
    while (offset + 4 <= len)
    {
        const uint8_t* p = data + offset;
        if (((p[0] >> 6) & 0x03) != 2)
        {
            return false;
        }

        const uint8_t count_or_fmt = p[0] & 0x1F;
        const uint8_t pt = p[1];
        const size_t pkt_len = (static_cast<size_t>(ReadUint16BE(p + 2)) + 1) * 4;
        if (pkt_len < 4 || offset + pkt_len > len)
        {
            return false;
        }

        if ((pt == 205 || pt == 206) && pkt_len >= 12)
        {
            ssrc = ReadUint32BE(p + 8);
            return true;
        }

        if (pt == 201 && count_or_fmt > 0 && pkt_len >= 32)
        {
            ssrc = ReadUint32BE(p + 8);
            return true;
        }

        if (pt == 201 && pkt_len >= 8)
        {
            ssrc = ReadUint32BE(p + 4);
            return true;
        }

        if (pt == 200 && count_or_fmt > 0 && pkt_len >= 52)
        {
            ssrc = ReadUint32BE(p + 28);
            return true;
        }

        if (pt == 200 && pkt_len >= 28)
        {
            ssrc = ReadUint32BE(p + 4);
            return true;
        }

        offset += pkt_len;
    }

    return false;
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
    if (!job.raw.data || job.raw.len == 0)
    {
        LOG_ERROR("invalid raw rtcp packet");
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleRtcpPacket(&pkt);
}

void MediaEndpoint::OnStun(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleStunPacket(&pkt);
}

void MediaEndpoint::OnDtls(WorkJob& job)
{
    if (!job.raw.data || job.raw.len == 0)
    {
        return;
    }

    Packet pkt;
    pkt.assign(job.raw.data, job.raw.len);
    pkt.enqueue_ts = job.enqueue_ts;
    HandleDtlsPacket(&pkt);
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

    uint32_t timestamp = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) | (uint32_t(data[6]) << 8)  | data[7];

    uint32_t ssrc = (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) | (uint32_t(data[10]) << 8) | data[11];

    TryGetRtpSsrc(pkt->data, pkt->len, ssrc);

    auto source_track = SourceTrack();
    auto source_session = SourceSession();
    if (source_track)
    {
        const auto& info = source_track->getTrackInfo();
        if (info.payload_type != 0xFF && payload_type != info.payload_type)
        {
            LOG_ERROR("[RTP] payload type mismatch, expected=", static_cast<int>(info.payload_type),
                      " actual=", static_cast<int>(payload_type),
                      " track=", TrackTypeToString(info.type),
                      " codec=", info.codec_name,
                      " ssrc=", ssrc);
            return;
        }

        if (source_session)
        {
            PayloadTypeInfo pt_info;
            if (source_session->FindPayloadType(payload_type, &pt_info))
            {
                const bool track_type_mismatch =
                    (info.type == TrackVideo && pt_info.track_type != StreamTrackType::Video) ||
                    (info.type == TrackAudio && pt_info.track_type != StreamTrackType::Audio);
                if (track_type_mismatch)
                {
                    LOG_ERROR("[RTP] SDP payload type track mismatch",
                              " pt=", static_cast<int>(payload_type),
                              " endpoint_track=", TrackTypeToString(info.type),
                              " sdp_codec=", pt_info.codec_name,
                              " ssrc=", ssrc);
                    return;
                }
            }
        }
    }

    auto track = GetOrCreateTrack(ssrc);
    if (!track)
    {
        return;
    }

    const auto& track_info = track->getTrackInfo();
    const int sample_rate = track_info.clock_rate > 0 ? static_cast<int>(track_info.clock_rate) : 90000;
    track->inputRtp(track->getTrackType(), sample_rate, pkt->data, pkt->len);
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
    auto source_track = SourceTrack();
    if (!source_track)
    {
        LOG_ERROR("[TRACK] source track not found, ssrc=", ssrc);
        return nullptr;
    }

    TrackInfo info = source_track->getTrackInfo();
    info.ssrc = ssrc;

    if (info.type != TrackVideo)
    {
        // LOG_INFO("[TRACK] ignore non-video RTP"," track=", TrackTypeToString(info.type), " codec=", info.codec_name, " pt=", static_cast<int>(info.payload_type), " ssrc=", ssrc);
        return nullptr;
    }

    if (info.codec_id != CodecId::H264)
    {
        LOG_ERROR("[TRACK] unsupported video codec for current depacketizer",
                  " codec=", info.codec_name,
                  " ssrc=", ssrc);
        return nullptr;
    }

    LOG_INFO("[TRACK] track not found, creating video track", " ssrc=", ssrc, " codec=", info.codec_name, " pt=", static_cast<int>(info.payload_type));

    auto new_track = std::make_shared<RtpVideoTracker>(info);

    LOG_INFO("[TRACK] create new track (pre-insert)", "ssrc=", ssrc, "track_ptr=", new_track.get());

    {
        std::lock_guard<std::mutex> lock(track_mtx_);

        auto it = ssrc_to_track_.find(ssrc);
        if (it != ssrc_to_track_.end())
        {
            LOG_ERROR("[TRACK] race detected, track already created by another thread", "ssrc=", ssrc, "exist_ptr=", it->second.get(), "new_ptr=", new_track.get());
            return it->second;
        }

        ssrc_to_track_[ssrc] = new_track;

        LOG_INFO("[TRACK] insert new track", "ssrc=", ssrc, "track_ptr=", new_track.get(), "total_tracks=", ssrc_to_track_.size());

        return new_track;
    }
}

void SfuEndpoint::HandleRtcpPacket(Packet* pkt)
{
    if (!pkt || !pkt->data || pkt->len < 4)
    {
        LOG_ERROR("invalid rtcp packet");
        return;
    }

    const uint8_t first_byte = pkt->data[0];
    const uint8_t version = (first_byte >> 6) & 0x03;
    const uint8_t count_or_fmt = first_byte & 0x1F;
    const uint8_t packet_type = pkt->data[1];
    const uint16_t length_words = ReadUint16BE(pkt->data + 2);
    LOG_INFO("[RTCP] HandleRtcpPacket enter",
             " len=", pkt->len,
             " version=", static_cast<int>(version),
             " pt=", static_cast<int>(packet_type),
             " count_or_fmt=", static_cast<int>(count_or_fmt),
             " length_words=", length_words);

    auto source_track = SourceTrack();
    if (source_track && source_track->getTrackInfo().type != TrackVideo)
    {
        const auto& info = source_track->getTrackInfo();
        LOG_INFO("[RTCP] ignore non-video RTCP", " track=", TrackTypeToString(info.type), " codec=", info.codec_name, " len=", pkt->len);
        return;
    }

    rtcpx::RtcpPacketInfo rtcp_info;
    if (!rtcpx::InspectRtcpPacket(pkt->data, pkt->len, &rtcp_info))
    {
        LOG_ERROR("[RTCP] invalid rtcp packet, len=", pkt->len);
        return;
    }

    if (!rtcp_info.has_media_ssrc)
    {
        LOG_ERROR("[RTCP] failed to get media ssrc",
                  " len=", pkt->len,
                  " pt=", static_cast<int>(rtcp_info.first_packet_type),
                  " fmt=", static_cast<int>(rtcp_info.first_count_or_fmt));
        return;
    }

    uint32_t media_ssrc = rtcp_info.media_ssrc;
    auto track = FindTrackBySsrc(media_ssrc);
    if (!track)
    {
        track = GetOrCreateTrack(media_ssrc);
        if (!track)
        {
            LOG_INFO("[RTCP] track not found yet, media_ssrc=", media_ssrc, " len=", pkt->len);
            return;
        }
    }

    track->inputRtcp(pkt->data, pkt->len);
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
    SetState(State::kRunning);
    return true;
}

void SfuEndpoint::Stop()
{
    std::lock_guard<std::mutex> lock(track_mtx_);
    ssrc_to_track_.clear();
    SetState(State::kStopped);
}
}
