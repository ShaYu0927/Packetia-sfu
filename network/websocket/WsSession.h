#ifndef _WS_SESSION_H_
#define _WS_SESSION_H_

#include "WsHeader.h"
#include <memory>
namespace network
{
/**
 * @brief 一个 WebSocket 连接对应一个 WsSession。
 *
 * WsSession 不负责会议业务，只保存连接上下文：
 * - sessionId
 * - roomId
 * - participantId
 * - WebSocket channel
 * - Room / Participant 弱引用
 */
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    using Ptr = std::shared_ptr<WsSession>;
    explicit WsSession(const std::string& connId, const WebSocketChannelPtr& channel);
    ~WsSession();

    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

public:
    const std::string& GetSessionId() const;

    void SetRoomId(const std::string& room_id);
    const std::string& GetRoomId() const;

    void SetParticipantId(const std::string& participant_id);
    const std::string& GetParticipantId() const;

    bool IsJoinedRoom() const;

    /**
     * @brief 发送文本信令消息。
     */
    bool SendText(const std::string& message);

    /**
     * @brief 关闭 WebSocket 连接。
     */
    void Close();

    void OnOpen();

    void OnMessage(const std::string& message);

    /**
     * @brief 清理房间和用户绑定关系。
     */
    void ClearBinding();

private:
    static std::string GenerateSessionId();

private:
    std::string session_id_;

    mutable std::mutex mutex_;

    std::string room_id_;
    std::string participant_id_;


    WebSocketChannelPtr channel_;
};
}


#endif /* _WS_SESSION_H_ */