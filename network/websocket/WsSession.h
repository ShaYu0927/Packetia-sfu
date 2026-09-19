#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace network
{
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

    WsSession(const std::string& connId, WakeCallback wake);
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
    bool PopOutgoing(std::string& message);
    bool NeedsWritable() const;
    bool IsClosing() const;

private:
    const std::string session_id_;
    mutable std::mutex mutex_;
    std::string room_id_, participant_id_, received_;
    std::deque<std::string> outgoing_;
    std::size_t queued_bytes_ = 0;
    bool open_ = false;
    bool closing_ = false;
    WakeCallback wake_;
    MessageCallback on_message_;
};
}
