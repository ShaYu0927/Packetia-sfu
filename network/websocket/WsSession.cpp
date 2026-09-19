#include "WsSession.h"
#include <utility>

namespace network
{
// All reservations from one session share this account. Closing it returns
// the aggregate once, including reservations held by the socket writer.
// Later destruction of those messages is then a no-op for accounting.
class WsSendAccount
{
public:
    WsSendAccount(const WsLimits& limits, std::shared_ptr<WsSendBudget> budget)
        : max_bytes_(limits.max_queued_bytes), max_messages_(limits.max_queued_messages),
          budget_(std::move(budget)) {}

    bool Reserve(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || used_.messages >= max_messages_ ||
            bytes > max_bytes_ - used_.bytes || !budget_->Reserve(bytes))
            return false;
        used_.bytes += bytes;
        ++used_.messages;
        return true;
    }

    void Release(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        used_.bytes -= bytes;
        --used_.messages;
        budget_->Release(bytes, 1);
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
        budget_->Release(used_.bytes, used_.messages);
        used_ = {};
    }

    WsSendStats GetStats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_;
    }

private:
    const std::size_t max_bytes_, max_messages_;
    std::shared_ptr<WsSendBudget> budget_;
    mutable std::mutex mutex_;
    WsSendStats used_;
    bool closed_ = false;
};

WsOutgoingMessage::WsOutgoingMessage(std::size_t bytes, std::shared_ptr<WsSendAccount> account) noexcept
    : account_(std::move(account)), reserved_bytes_(bytes) {}
WsOutgoingMessage::~WsOutgoingMessage() { Reset(); }
WsOutgoingMessage::WsOutgoingMessage(WsOutgoingMessage&& other) noexcept
    : data_(std::move(other.data_)), account_(std::move(other.account_)),
      reserved_bytes_(other.reserved_bytes_) {}
WsOutgoingMessage& WsOutgoingMessage::operator=(WsOutgoingMessage&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        data_ = std::move(other.data_);
        account_ = std::move(other.account_);
        reserved_bytes_ = other.reserved_bytes_;
    }
    return *this;
}
void WsOutgoingMessage::Reset()
{
    std::string{}.swap(data_);
    if (account_) account_->Release(reserved_bytes_);
    account_.reset();
    reserved_bytes_ = 0;
}

WsSession::WsSession(const std::string& connId, WakeCallback wake,
                     WsLimits limits, std::shared_ptr<WsSendBudget> budget)
    : session_id_(connId), limits_(limits),
      send_account_(std::make_shared<WsSendAccount>(limits, budget ? std::move(budget) :
          std::make_shared<WsSendBudget>(limits.max_total_queued_bytes,
                                         limits.max_total_queued_messages))),
      wake_(std::move(wake)) {}

WsSession::~WsSession()
{
    outgoing_.clear();
    send_account_->Close();
}

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
        if (!open_ || closing_ || message.size() > limits_.max_message_bytes)
            return false;
        // Reject exhausted budgets without copying the payload. The ticket
        // already owns the full reservation if allocation or enqueue throws.
        if (!send_account_->Reserve(message.size())) return false;
        WsOutgoingMessage outgoing(message.size(), send_account_);
        outgoing.data_ = message;
        const bool was_empty = outgoing_.empty();
        outgoing_.push_back(std::move(outgoing));
        if (was_empty) wake = wake_;
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
        outgoing_.clear();
        send_account_->Close();
        received_.clear();
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
    if (!closing_) open_ = true;
}
void WsSession::OnClosed()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    closing_ = true;
    received_.clear();
    outgoing_.clear();
    send_account_->Close();
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
        if (!open_ || closing_ || size > limits_.max_message_bytes - received_.size() ||
            (size != 0 && data == nullptr)) return false;
        if (size) received_.append(static_cast<const char*>(data), size);
        if (!final) return true;
        message.swap(received_);
    }
    OnMessage(message);
    return true;
}
bool WsSession::PopOutgoing(WsOutgoingMessage& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || closing_ || outgoing_.empty()) return false;
    message = std::move(outgoing_.front());
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
WsSendStats WsSession::GetSendStats() const { return send_account_->GetStats(); }
}
