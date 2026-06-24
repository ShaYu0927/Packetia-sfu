#include "WsSession.h"
#include "logger.h"
#include <chrono>

namespace network 
{

static std::atomic<uint64_t> g_session_index{0};

WsSession::WsSession(const std::string& connId, const WebSocketChannelPtr& channel)
    : session_id_(connId),
      channel_(channel)
{
}

WsSession::~WsSession() = default;

std::string WsSession::GenerateSessionId()
{
    uint64_t index = ++g_session_index;

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    return "ws_" + std::to_string(ms) + "_" + std::to_string(index);
}

const std::string& WsSession::GetSessionId() const
{
    return session_id_;
}

void WsSession::SetRoomId(const std::string& room_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    room_id_ = room_id;
}

const std::string& WsSession::GetRoomId() const
{
    return room_id_;
}

void WsSession::SetParticipantId(const std::string& participant_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    participant_id_ = participant_id;
}

const std::string& WsSession::GetParticipantId() const
{
    return participant_id_;
}

bool WsSession::IsJoinedRoom() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !room_id_.empty() && !participant_id_.empty();
}

bool WsSession::SendText(const std::string& message)
{
    if (!channel_)
    {
        return false;
    }

    channel_->send(message);
    return true;
}

void WsSession::Close()
{
    if (!channel_)
    {
        return;
    }

    channel_->close();
}

void WsSession::ClearBinding()
{
    std::lock_guard<std::mutex> lock(mutex_);

    room_id_.clear();
    participant_id_.clear();
}

void WsSession::OnOpen()
{
    
}

void WsSession::OnMessage(const std::string& message)
{
    LOG_INFO("wssession::onmessage" + message);
}

} // namespace network