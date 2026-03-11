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

