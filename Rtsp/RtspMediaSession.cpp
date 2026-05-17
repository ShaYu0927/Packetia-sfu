#include "RtspMediaSession.h"
#include "SdpMode.h"
#include "RtspUtil.h"
#include "StreamContext.h"

static uint64_t MakeStreamKey(uint32_t session_id, uint32_t track_id)
{
    return (static_cast<uint64_t>(session_id) << 32) |
           static_cast<uint32_t>(track_id);
}

MediaSession::Ptr MediaSession::CreateNew(const std::string& suffix)
{
    auto session = std::make_shared<MediaSession>();
    session->suffix_ = suffix;
    return session;
}

std::string MediaSessionManager::GenerateGlobalId()
{
    static std::atomic<uint64_t> seq{0};

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    uint64_t n = ++seq;

    std::ostringstream oss;
    oss << now << "-" << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << "-" << n;
    return oss.str();
}

uint32_t MediaSessionManager::AddSession(MediaSession::Ptr session, const std::string& suffix) 
{
    if (!session)
        return 0;

    std::lock_guard<std::mutex> lock(mtx_);

    if (suffix.empty())
        return 0;

    if (suffix_map_.find(suffix) != suffix_map_.end())
        return 0;

    uint32_t id = ++last_id_;
    session->session_id_ = id;

    if (session->global_id_.empty())
        session->global_id_ = GenerateGlobalId();

    sessions_[id] = session;
    suffix_map_[suffix] = session;
    return id;
}

MediaSession::Ptr MediaSessionManager::GetSessionById(const uint32_t& id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

MediaSession::Ptr MediaSessionManager::GetSessionBySuffix(const std::string& suffix)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = suffix_map_.find(suffix);
    return (it != suffix_map_.end()) ? it->second : nullptr;
}

void MediaSessionManager::RemoveSession(const uint32_t& id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) 
    {
        for (auto sit = suffix_map_.begin(); sit != suffix_map_.end();) 
        {
            if (sit->second == it->second) 
            {
                sit = suffix_map_.erase(sit);
            } 
            else 
            {
                ++sit;
            }
        }
        sessions_.erase(it);
    }
}

void MediaSession::ResetTracks()
{
    track_infos_.clear();
    control_to_track_.clear();
    runtime_tracks_.clear();
    sdp_.clear();
}

bool MediaSession::ParseTrackInfoFromMedia(const sdp::SdpMedia& media, int track_index, TrackInfo* info, std::string* err) const
{
      if (!info)
    {
        if (err)
        {
            *err = "invalid TrackInfo output";
        }
        return false;
    }

    *info = TrackInfo{};
    info->track_index = track_index;

    if (media.media == "audio")
    {
        info->type = TrackType::TrackAudio;
    }
    else if (media.media == "video")
    {
        info->type = TrackType::TrackVideo;
    }
    else
    {
        info->type = TrackType::TrackInvalid;
        if (err)
        {
            *err = "unsupported media type: " + media.media;
        }
        return false;
    }

    info->control = media.GetAttribute("control");

    if (!media.fmtps.empty())
    {
        info->fmtp = media.fmtps[0].params;
    }
    else
    {
        std::string raw_fmtp = media.GetAttribute("fmtp");
        if (!raw_fmtp.empty())
        {
            info->fmtp = rtsp::RtspUtil::StripFmtpPayloadPrefix(raw_fmtp);
        }
    }

    bool got_rtpmap = false;

    if (!media.rtpmaps.empty())
    {
        const auto& rtpmap = media.rtpmaps[0];

        info->payload_type = static_cast<uint8_t>(rtpmap.payloadType);
        info->codec_name   = rtpmap.encodingName;
        info->codec_id     = StringToCodecId(info->codec_name);
        info->clock_rate   = static_cast<uint32_t>(rtpmap.clockRate);
        info->channels     = rtpmap.channels;
        got_rtpmap = true;
    }
    else
    {
        std::string raw_rtpmap = media.GetAttribute("rtpmap");
        if (!raw_rtpmap.empty())
        {
            int payload_type = -1;
            std::string codec_name;
            uint32_t clock_rate = 0;
            int channels = 0;

            if (rtsp::RtspUtil::ParseRtpMapLine(raw_rtpmap,
                                &payload_type,
                                &codec_name,
                                &clock_rate,
                                &channels))
            {
                info->payload_type = static_cast<uint8_t>(payload_type);
                info->codec_name   = codec_name;
                info->codec_id     = StringToCodecId(info->codec_name);
                info->clock_rate   = clock_rate;
                info->channels     = channels;
                got_rtpmap = true;
            }
        }
    }

    if (!got_rtpmap)
    {
        if (err)
        {
            *err = "missing rtpmap for track index " + std::to_string(track_index);
        }
        return false;
    }

    if (info->codec_id == CodecId::Unknown)
    {
        if (err)
        {
            *err = "unsupported codec '" + info->codec_name +
                   "' for track index " + std::to_string(track_index);
        }
        return false;
    }

    if (info->clock_rate == 0)
    {
        if (const auto* traits = GetCodecTraits(info->codec_id))
        {
            info->clock_rate = traits->default_clock_rate;
        }
    }

    if (info->clock_rate == 0)
    {
        if (err)
        {
            *err = "invalid clock rate for track index " + std::to_string(track_index);
        }
        return false;
    }

    if (info->control.empty())
    {
        info->control = "trackID=" + std::to_string(track_index);
    }

    return true;
}


RtpTrack::Ptr MediaSession::CreateTrack(const TrackInfo& info)
{
    switch (info.type)
    {
    case TrackType::TrackAudio:
        switch (info.codec_id)
        {
        case CodecId::PCMU:
        case CodecId::PCMA:
        case CodecId::OPUS:
        case CodecId::AAC:
            return std::make_shared<AudioTrack>(info);

        default:
            return nullptr;
        }

    case TrackType::TrackVideo:
        switch (info.codec_id)
        {
        case CodecId::H264:
        case CodecId::H265:
            return std::make_shared<VideoTrack>(info);

        default:
            return nullptr;
        }

    default:
        return nullptr;
    }
}

bool MediaSession::ApplySdp(const sdp::SdpSession& sdp, std::string* err)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    ResetTracks();
    auto ctx = StreamContextBuilder::BuildFromSdp(sdp, suffix_, url_);

    int track_idx = 0;
    for (const auto& media : sdp.medias)
    {
        if (track_idx >= MAX_TRACKS)
        {
            if (err)
            {
                *err = "too many tracks";
            }
            return false;
        }

        TrackInfo info;
        if (!ParseTrackInfoFromMedia(media, track_idx, &info, err))
        {
            return false;
        }

        if (info.type == TrackType::TrackInvalid)
        {
            continue;
        }

        auto track = BuildTrackFromInfo(info, err);
        if (!track)
        {
            return false;
        }

        LOG_INFO("track_idx=", track_idx,
                 " control=", info.control,
                 " codec=", info.codec_name,
                 " pt=", static_cast<int>(info.payload_type),
                 " clock_rate=", info.clock_rate);

        track_infos_[track_idx] = info;
        control_to_track_[info.control] = track_idx;
        runtime_tracks_[track_idx] = track;

        ++track_idx;
    }
    return true;
}

RtpTrack::Ptr MediaSession::BuildTrackFromInfo(const TrackInfo& info, std::string* err)
{
    return CreateTrack(info);
}


std::shared_ptr<RtpTrack> MediaSession::GetRtpTrack(const std::string& control) const
{
    auto it = control_to_track_.find(control);
    if (it == control_to_track_.end())
        return nullptr;

    int trackIdx = it->second;

    auto track_it = runtime_tracks_.find(trackIdx);
    if (track_it == runtime_tracks_.end())
        return nullptr;

    return track_it->second;
}

bool MediaSession::BindInterleavedChannel(uint8_t channel, int track_id, bool is_rtcp, uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lk(track_mtx_);

    auto it = runtime_tracks_.find(track_id);
    if (it == runtime_tracks_.end())
        return false;

    ChannelBinding& b = channel_bindings_[channel];
    b.valid = true;
    b.track_id = track_id;
    b.is_rtcp = is_rtcp;
    b.key = MakeStreamKey(session_id_, static_cast<uint32_t>(track_id));
    b.endpoint_id = endpoint_id;
    return true;
}


bool MediaSession::GetChannelBinding(uint8_t channel, ChannelBinding* out) const
{
    if (!out) return false;

    std::lock_guard<std::mutex> lk(track_mtx_);
    const auto& b = channel_bindings_[channel];
    if (!b.valid) return false;

    *out = b;
    return true;
}

bool MediaSession::BindTrackEndpoint(int track_id, uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    endpoint_to_track_[endpoint_id] = track_id;
    return true;
}

uint64_t MediaSession::FindEndpointByTrack(int track_id) const
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    auto it = std::find_if(endpoint_to_track_.begin(), endpoint_to_track_.end(),
        [track_id](const auto& pair) { return pair.second == track_id; });
    return (it != endpoint_to_track_.end()) ? it->first : 0;
}
