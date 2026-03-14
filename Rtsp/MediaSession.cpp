#include "MediaSession.h"
#include "SdpMode.h"

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

    MediaSessionId id = ++last_id_;
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

bool MediaSession::ApplySdp(const sdp::SdpSession& sdp, std::string* err)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    track_infos_.clear();
    control_to_track_.clear();
    sdp_.clear();

    int trackIdx = 0;
    for (const sdp::SdpMedia& media : sdp.medias)
    {
        TrackType type = TrackType::TrackInvalid;
        if (media.media == "audio")
            type = TrackType::TrackAudio;
        else if (media.media == "video")
            type = TrackType::TrackVideo;
        else
            continue;

        if (trackIdx >= MAX_TRACKS)
        {
            if (err) *err = "too many tracks";
            return false;
        }

        MediaTrackInfo info;
        info.valid = true;
        info.type = type;
        info.control = media.GetAttribute("control");
        info.fmtp = media.GetAttribute("fmtp");

        if (!media.fmtps.empty())
        {
            info.fmtp = media.fmtps[0].params;
        }

        if (!media.rtpmaps.empty())
        {
            const auto& rtpmap = media.rtpmaps[0];
            info.payload_type = rtpmap.payloadType;
            info.codec = rtpmap.encodingName;
            info.clock_rate = rtpmap.clockRate;
            info.channels = rtpmap.channels;
        }
     

        if (info.control.empty())
        {
            info.control = "trackID=" + std::to_string(trackIdx);
        }

        track_infos_[trackIdx] = info;
        control_to_track_[info.control] = trackIdx;
        ++trackIdx;
    }

    // if (track_infos_.empty())
    // {
    //     if (err) *err = "no valid media track in sdp";
    //     return false;
    // }

    return true;
}

