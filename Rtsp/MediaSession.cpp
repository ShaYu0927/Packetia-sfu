#include "MediaSession.h"

MediaSession::Ptr MediaSession::CreateNew(const std::string& suffix)
{
    return nullptr;
}

std::string MediaSessionManager::AddSession(MediaSession::Ptr session, const std::string& suffix) 
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::string id = std::to_string(++last_id_);
    session->session_id_ = last_id_;
    sessions_[id] = session;
    suffix_map_[suffix] = session;
    return id;
}

MediaSession::Ptr MediaSessionManager::GetSessionById(const std::string& id)
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

void MediaSessionManager::RemoveSession(const std::string& id)
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
    for (const auto& media : sdp.medias)
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

    if (track_infos_.empty())
    {
        if (err) *err = "no valid media track in sdp";
        return false;
    }

    return true;
}

