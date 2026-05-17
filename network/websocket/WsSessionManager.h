#ifndef _WS_SESSION_MANAGER_H_
#define _WS_SESSION_MANAGER_H_

#include "WsSession.h"

namespace network
{
class WsSessionManager
{
public:
    using SessionPtr = std::shared_ptr<WsSession>;

    bool AddSession(const SessionPtr& session);
    void RemoveSession(const std::string& session_id);
    SessionPtr GetSession(const std::string& session_id) const;
    size_t GetSessionCount() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionPtr> sessions_;
};
}


#endif /* _WS_SESSION_MANAGER_H_ */