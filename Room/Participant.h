#ifndef _PARTICIPANT_H_
#define _PARTICIPANT_H_

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "TrackeInfo.h"

class MediaSession;

namespace room
{

/**
 * @brief 参会人状态。
 *
 * ParticipantState 用于描述一个用户在会议中的生命周期状态。
 */
enum class ParticipantState
{
    Joining = 0,     // 正在加入
    Joined,          // 已加入，但媒体链路可能还未完全建立
    Active,          // 已激活，可以发布或订阅媒体流
    Disconnected,    // 已断开
};

/**
 * @brief 会议参会人对象。
 *
 * Participant 表示会议房间中的一个用户。
 *
 * 它不直接处理 RTP 包，也不直接负责网络收发。
 * 它主要负责维护用户身份、用户状态、绑定的 MediaSession、
 * 已发布的 Track、已订阅的 Track，以及与 Room 之间的事件回调。
 *
 * 在整体架构中的位置：
 *
 * Room
 *   └── Participant
 *         ├── MediaSession
 *         ├── Published Tracks
 *         └── Subscribed Tracks
 */
class Participant : public std::enable_shared_from_this<Participant>
{
public:
    using Ptr = std::shared_ptr<Participant>;

    using TrackCallback = std::function<void(Participant::Ptr, media::MediaTrackPtr)>;
    using StateCallback = std::function<void(Participant::Ptr)>;
    using LeaveCallback = std::function<void(Participant::Ptr)>;

public:
    Participant(std::string participant_id, std::string name);

    ~Participant();

    /**
     * @brief 获取参会人 ID。
     */
    std::string Id() const;

    /**
     * @brief 获取参会人名称。
     */
    std::string Name() const;

    /**
     * @brief 设置参会人名称。
     */
    void SetName(const std::string& name);

    /**
     * @brief 绑定媒体会话。
     *
     * MediaSession 表示该用户的一次连接会话
     * 一个 Participant 可以绑定一个当前有效的 MediaSession。
     */
    void BindSession(std::shared_ptr<MediaSession> session);

    /**
     * @brief 获取当前绑定的 MediaSession。
     */
    std::shared_ptr<MediaSession> GetSession() const;

    /**
     * @brief 设置参会人状态。
     */
    void SetState(ParticipantState state);

    /**
     * @brief 获取参会人状态
     */
    ParticipantState State() const;

    /**
     * @brief 判断是否已经断开
     */
    bool IsDisconnected() const;

    /**
     * @brief 判断是否处于可用状态
     */
    bool IsActive() const;

    /**
     * @brief 添加已发布 Track。
     *
     * 用户发布摄像头、麦克风、屏幕共享时，
     * 可以把对应的 MediaTrack 添加到 Participant 中
     */
    bool AddPublishedTrack(const media::MediaTrackPtr& track);

    /**
     * @brief 移除已发布 Track。
     */
    bool RemovePublishedTrack(const std::string& track_id);

    /**
     * @brief 根据 Track ID 查找已发布 Track。
     */
    media::MediaTrackPtr GetPublishedTrack(const std::string& track_id) const;

    /**
     * @brief 获取当前用户发布的所有 Track。
     */
    std::vector<media::MediaTrackPtr> GetPublishedTracks() const;

    /**
     * @brief 添加订阅 Track。
     *
     * 这里先只保存订阅关系，后面可以再接 RtpSenderTrack / DownTrack。
     */
    bool SubscribeTrack(const std::string& track_id);

    /**
     * @brief 取消订阅 Track。
     */
    bool UnsubscribeTrack(const std::string& track_id);

    /**
     * @brief 判断是否已经订阅某个 Track。
     */
    bool HasSubscribedTrack(const std::string& track_id) const;

    /**
     * @brief 获取所有已订阅的 Track ID。
     */
    std::vector<std::string> GetSubscribedTrackIds() const;

    /**
     * @brief 主动离开会议。
     */
    void Leave();

    /**
     * @brief 设置 Track 发布回调。
     */
    void OnTrackPublished(TrackCallback cb);

    /**
     * @brief 设置 Track 取消发布回调。
     */
    void OnTrackUnpublished(TrackCallback cb);

    /**
     * @brief 设置状态变化回调。
     */
    void OnStateChanged(StateCallback cb);

    /**
     * @brief 设置离开会议回调。
     */
    void OnLeave(LeaveCallback cb);

private:
    TrackCallback GetOnTrackPublished() const;
    TrackCallback GetOnTrackUnpublished() const;
    StateCallback GetOnStateChanged() const;
    LeaveCallback GetOnLeave() const;

private:
    mutable std::mutex mutex_;

    std::string participant_id_;
    std::string name_;

    ParticipantState state_ = ParticipantState::Joining;

    std::shared_ptr<MediaSession> session_;

    std::unordered_map<std::string, media::MediaTrackPtr> published_tracks_;
    std::unordered_set<std::string> subscribed_track_ids_;

    TrackCallback on_track_published_;
    TrackCallback on_track_unpublished_;
    StateCallback on_state_changed_;
    LeaveCallback on_leave_;
};

} // namespace room




#endif // _PARTICIPANT_H_