#include "websocket/WsSessionManager.h"

namespace network
{
bool WsSessionManager::AddSession(const SessionPtr& session, std::size_t max_sessions)
{
    if (!session)
    {
        return false;
    }

    const std::string& session_id = session->GetSessionId();
    if (session_id.empty())
    {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (sessions_.size() >= max_sessions) return false;

    auto ret = sessions_.emplace(session_id, session);
    return ret.second;
}

void WsSessionManager::RemoveSession(const std::string& session_id)
{
    SessionPtr session;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto it = sessions_.find(session_id);
        if (it == sessions_.end())
        {
            return;
        }

        session = it->second;
        sessions_.erase(it);
    }

    if (session)
    {
        session->ClearBinding();
        session->Close();
    }
}

WsSessionManager::SessionPtr
WsSessionManager::GetSession(const std::string& session_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end())
    {
        return nullptr;
    }

    return it->second;
}

size_t WsSessionManager::GetSessionCount() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return sessions_.size();
}


}
