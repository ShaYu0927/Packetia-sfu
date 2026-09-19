#pragma once

#include "WsLimits.h"
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace network
{
// Keep this object until the complete WebSocket message has been written.
// Moving it transfers the reservation; Reset or destruction returns it.
class WsOutgoingMessage
{
public:
    WsOutgoingMessage() = default;
    ~WsOutgoingMessage();
    WsOutgoingMessage(WsOutgoingMessage&& other) noexcept;
    WsOutgoingMessage& operator=(WsOutgoingMessage&& other) noexcept;
    WsOutgoingMessage(const WsOutgoingMessage&) = delete;
    WsOutgoingMessage& operator=(const WsOutgoingMessage&) = delete;

    const std::string& Data() const { return data_; }
    std::size_t Size() const { return data_.size(); }
    // An empty text frame still has a reservation and is not Empty().
    bool Empty() const { return !account_; }
    void Reset();

private:
    friend class WsSession;
    WsOutgoingMessage(std::size_t bytes, std::shared_ptr<WsSendAccount> account) noexcept;
    std::string data_;
    std::shared_ptr<WsSendAccount> account_;
    std::size_t reserved_bytes_ = 0;
};

// Workers enqueue messages; only WsServer's service thread touches sockets.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    using Ptr = std::shared_ptr<WsSession>;
    using MessageCallback = std::function<void(const std::string&, const std::string&)>;
    using CloseCallback = std::function<void(const std::string&)>;
    using WakeCallback = std::function<void()>;
    static constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
    static constexpr std::size_t kMaxQueuedBytes = 4 * kMaxMessageBytes;
    static constexpr std::size_t kMaxQueuedMessages = 256;

    WsSession(const std::string& connId, WakeCallback wake,
              WsLimits limits = {}, std::shared_ptr<WsSendBudget> budget = {});
    ~WsSession();
    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    const std::string& GetSessionId() const;
    void SetRoomId(const std::string& room_id);
    std::string GetRoomId() const;
    void SetParticipantId(const std::string& participant_id);
    std::string GetParticipantId() const;
    bool IsJoinedRoom() const;
    bool SendText(const std::string& message);
    void Close();
    void ClearBinding();
    void SetOnMessage(MessageCallback cb);
    void OnMessage(const std::string& message);

    // Service-thread entry points. Callbacks run without the session lock.
    void OnOpen();
    void OnClosed();
    bool ReceiveFragment(const void* data, std::size_t size, bool final);
    bool PopOutgoing(WsOutgoingMessage& message);
    bool NeedsWritable() const;
    bool IsClosing() const;
    WsSendStats GetSendStats() const;

private:
    const std::string session_id_;
    const WsLimits limits_;
    std::shared_ptr<WsSendAccount> send_account_;
    mutable std::mutex mutex_;
    std::string room_id_, participant_id_, received_;
    std::deque<WsOutgoingMessage> outgoing_;
    bool open_ = false;
    bool closing_ = false;
    WakeCallback wake_;
    MessageCallback on_message_;
};
}
