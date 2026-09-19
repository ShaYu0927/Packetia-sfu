#include "WsSession.h"
#include <utility>

namespace network
{
WsSession::WsSession(const std::string& connId, WakeCallback wake)
    : session_id_(connId), wake_(std::move(wake)) {}

const std::string& WsSession::GetSessionId() const { return session_id_; }

void WsSession::SetRoomId(const std::string& room_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    room_id_ = room_id;
}
std::string WsSession::GetRoomId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return room_id_;
}
void WsSession::SetParticipantId(const std::string& participant_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    participant_id_ = participant_id;
}
std::string WsSession::GetParticipantId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return participant_id_;
}
bool WsSession::IsJoinedRoom() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !room_id_.empty() && !participant_id_.empty();
}

bool WsSession::SendText(const std::string& message)
{
    WakeCallback wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || closing_ || message.size() > kMaxMessageBytes ||
            outgoing_.size() >= kMaxQueuedMessages ||
            message.size() > kMaxQueuedBytes - queued_bytes_)
            return false;
        outgoing_.push_back(message);
        queued_bytes_ += message.size();
        wake = wake_;
    }
    if (wake) wake();
    return true;
}
void WsSession::Close()
{
    WakeCallback wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || closing_) return;
        closing_ = true;
        wake = wake_;
    }
    if (wake) wake();
}
void WsSession::ClearBinding()
{
    std::lock_guard<std::mutex> lock(mutex_);
    room_id_.clear();
    participant_id_.clear();
}
void WsSession::SetOnMessage(MessageCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(cb);
}
void WsSession::OnMessage(const std::string& message)
{
    MessageCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || closing_) return;
        callback = on_message_;
    }
    if (callback) callback(session_id_, message);
}
void WsSession::OnOpen()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
}
void WsSession::OnClosed()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    closing_ = true;
    received_.clear();
    outgoing_.clear();
    queued_bytes_ = 0;
    room_id_.clear();
    participant_id_.clear();
    on_message_ = {};
    wake_ = {};
}
bool WsSession::ReceiveFragment(const void* data, std::size_t size, bool final)
{
    std::string message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || closing_ || size > kMaxMessageBytes - received_.size()) return false;
        if (size) received_.append(static_cast<const char*>(data), size);
        if (!final) return true;
        message.swap(received_);
    }
    OnMessage(message);
    return true;
}
bool WsSession::PopOutgoing(std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || closing_ || outgoing_.empty()) return false;
    message = std::move(outgoing_.front());
    queued_bytes_ -= message.size();
    outgoing_.pop_front();
    return true;
}
bool WsSession::NeedsWritable() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && (closing_ || !outgoing_.empty());
}
bool WsSession::IsClosing() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_ || !open_;
}
}
