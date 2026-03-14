#include "Rtsp.h"
#include "MediaSession.h"
#include "logger.h"


std::shared_ptr<MediaSession> Rtsp::LookMediaSession(const std::string& suffix)
{
    auto it = media_sessions_.find(suffix);
    if (it != media_sessions_.end())
        return it->second;
    return nullptr;
}

std::shared_ptr<MediaSession> Rtsp::LookMediaSession(MediaSessionId sessionId)
{
    for (auto& kv : media_sessions_)
    {
        if (kv.second->GetId() == sessionId)
            return kv.second;
    }
    return nullptr;
}


bool Rtsp::AddMediaSession(const std::shared_ptr<MediaSession>& session)
{
     const auto& suffix = session->GetRtspSuffix();
    if (media_sessions_.find(suffix) != media_sessions_.end()) {
        LOG_INFO("MediaSession already exists: " + suffix);
        return false;
    }

    media_sessions_[suffix] = session;
    LOG_INFO("Added MediaSession: " + suffix);
    return true;
}


