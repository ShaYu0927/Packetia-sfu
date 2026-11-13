#ifndef PACKETIA_MEDIA_SFU_ENDPOINT_H_
#define PACKETIA_MEDIA_SFU_ENDPOINT_H_

#include "EndpointBase.h"
#include "RtpReceiver.h"
#include "RtcpContext.h"
#include "EncodedFrameRouter.h"
#include "SfuEndpointTypes.h"
#include "DownlinkTrackManager.h"
#include "EncodedFrameDispatcher.h"
#include "ReceiveFeedbackHandler.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace sdp { struct SdpMedia; }

namespace media 
{

/**
 * SFU 媒体端点：协调发布轨道、RTP/RTCP 入口和跨端点转发。
 *
 * 下行关系及发送器交给 DownlinkTrackManager，编码帧交给 EncodedFrameDispatcher，
 * 接收反馈由 ReceiveFeedbackHandler 衔接 quality 模块。轨道变更与收包通过 runtime_mutex_ 串行化。
 * 轨道 ID 保持稳定，MID、SSRC 记录当前媒体会话内的协商绑定。
 * 完整编码帧通过回调和帧路由提供给录制、转码等消费者。
 */
class SfuEndpoint : public utils::EndpointBase, public std::enable_shared_from_this<SfuEndpoint>
{
public:
    SfuEndpoint(uint64_t id,
                std::shared_ptr<RtpTrackDescription> track_description,
                const EncodedFrameSource& source,
                std::shared_ptr<IEncodedFramePublisher> frame_publisher);
    explicit SfuEndpoint(uint64_t id, std::shared_ptr<IEncodedFramePublisher> publisher = {})
        : utils::EndpointBase(id, "SfuEndpoint"), frames_(id, {}, std::move(publisher)) {}
    ~SfuEndpoint() override { Stop(); }
    using SendRtcpCallback = endpoint::SendRtcpCallback;
    using EncodedFrameCallback = EncodedFrameDispatcher::Callback;
    using FrameSubscriptionId = EncodedFrameDispatcher::SubscriptionId;

    // 注册发布轨道；可直接提供接收描述，或从选定的 SDP 媒体项构建。
    bool AddPublishedTrack(const std::string& id, std::shared_ptr<RtpTrackDescription> description, std::string mid = {}, uint8_t mid_extension_id = 0);
    bool AddPublishedTrack(const std::string& id, const sdp::SdpMedia& media, int track_index);

    // 按完整协商结果更新发布集合：MID 唯一且非空，每项只接受一种选定编码。
    // 保留可复用的接收器、轨道索引和订阅；有活动订阅时拒绝不兼容的编码或源切换。
    // 空集合暂停全部轨道；暂停保留 MID、索引及 RTCP 配置，并解除下游订阅。
    // 已使用的 SSRC 不允许转移到其他轨道。
    bool ReplacePublishedTracks(const std::vector<sdp::SdpMedia>& medias);

    // 移除发布轨道；retire_identity=false 仅用于回滚尚未发布的 SETUP。
    bool RemovePublishedTrack(const std::string& id, bool retire_identity = true);

    // 将接收 SSRC 绑定到发布轨道，拒绝占用其他轨道的来源。
    bool BindSsrc(const std::string& id, uint32_t ssrc);
    bool GetPublishedTrack(const std::string& id, ::TrackInfo& info) const;
    size_t PublishedTrackCount() const;
    size_t SubscriptionCount() const;
    // Present once the RTP receiver is created, including before its first SR.
    std::optional<NetworkStatsSnapshot> GetReceiveQualitySnapshot(uint32_t source_ssrc) const;

    // 设置单轨道 RTCP 出口；未设置时使用端点默认出口。
    void SetTrackRtcpSendCallback(const std::string& id, SendRtcpCallback cb);

    // 先配置订阅者协商出的下行参数，再由 Room::SubscribeTrack 建立关系。
    // 输出 PT 和头扩展编号以订阅者协商结果为准。
    bool ConfigureSubscription(const std::string& id, const rtsp::RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport, CodecId codec);

    // 当前端点订阅 source 的指定轨道，创建独立发送器和可选的全局消费闸门。
    bool Subscribe(const std::string& id, const std::shared_ptr<SfuEndpoint>& source, const std::string& source_track_id, MediaTrackRef track = {});

    // 关闭转发关系并回收发送器，保留下行配置供再次订阅。
    bool Unsubscribe(const std::string& id);
    // 保留协商和路由，只控制本端音频消费者。
    bool SetAudioSubscriptionMuted(const std::string& id, bool muted, bool& changed);
    bool SetSubscriptionBandwidthPolicy(const std::string& id, const DownlinkSubscriptionPolicy& policy);
    std::optional<endpoint::DownlinkBandwidthStatus> GetSubscriptionBandwidthStatus(const std::string& id) const;

    // 媒体任务入口，处理当前会话的 RTP 数据和 RTCP 报告/反馈。
    void OnRtp(WorkJob& job) override;
    void OnRtcp(WorkJob& job) override;

    // 初始化反馈分发器；停止时回收接收器、订阅与媒体状态。
    bool Start() override;
    void Stop() override;

    // 在接收流所属 Worker 上推进无新包时的排序与组帧期限检查。
    bool PollMedia(uint64_t stream_key, uint64_t now_ms) override;

    // 按 SSRC 移除接收流，保留已协商的逻辑轨道。
    int RemoveMediaStream(uint32_t source_ssrc);

    // 处理接收侧补包与关键帧请求，关键帧预算由发布轨道共享。
    void OnTrackNack(uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs);
    void OnTrackPli(uint32_t media_ssrc);

    // 在媒体 Worker 上推进恢复；ScheduleRecoveryTick 负责异步投递。
    // 无新 RTP 到达时仍可处理补包重试及超时。
    void TickRecovery();
    bool ScheduleRecoveryTick();

    // 设置默认 RTCP 出口，供没有专用回调的发布轨道使用。
    void SetRtcpSendCallback(SendRtcpCallback cb);

    // 编码帧回调同步执行，必须快速返回；移除不等待已开始的回调。
    FrameSubscriptionId AddEncodedFrameCallback(EncodedFrameCallback cb);
    void RemoveEncodedFrameCallback(FrameSubscriptionId id);

protected:
    void HandleRtpPacket(common::BufferView packet);
    void HandleRtcpPacket(common::BufferView packet);

    void ForwardRtpToSubscribers(uint32_t source_ssrc, const uint8_t* data, size_t len);

    // RTSP/WebRTC 接入层提供的来源标识，随编码帧传给下游业务。
    uint32_t media_stream_id_ = 0;

private:
    using Subscription = endpoint::Subscription;
    using PublishedTrack = endpoint::PublishedTrack;
    using PausedPublishedTrack = endpoint::PausedPublishedTrack;

    bool ResolveRtpTrack(common::BufferView packet, const std::string& hint);
    SendRtcpCallback RtcpSenderFor(uint32_t ssrc);
    void Deliver(const std::shared_ptr<Subscription>& subscription, common::SharedBuffer packet);
    void RequestKeyFrame(const std::string& track_id);
    void PruneSubscriptions();
    void UpdateDownlinkBandwidth();

    // 发布身份、协商绑定和转发关系；控制操作与媒体任务共用此锁。
    mutable std::recursive_mutex runtime_mutex_;
    std::unordered_map<std::string, PublishedTrack> published_tracks_;
    std::unordered_map<std::string, PausedPublishedTrack> paused_published_tracks_;
    std::unordered_map<uint32_t, std::string> ssrc_bindings_;

    // 退役身份阻止迟到数据包重新建立已移除的轨道绑定。
    std::unordered_set<uint32_t> retired_ssrcs_;
    std::unordered_set<std::string> retired_track_ids_;
    rtsp::RtpReceiverTrack::Ptr FindReceiverTrackBySsrc(uint32_t ssrc);
    std::shared_ptr<rtsp::RtpReceiverTrack> GetOrCreateReceiverTrack(uint32_t ssrc);
    void RemoveMediaStreamOnOwner(uint32_t source_ssrc);
    void DispatchEncodedFrame(const media::EncodedFrame::Ptr& frame);
    void EvaluateReceiveQuality(uint32_t source_ssrc);

private:
    // 接收器及超时轮询索引；完整编码帧暂存后统一分发。
    std::mutex track_mtx_;
    std::unordered_map<uint32_t, rtsp::RtpReceiverTrack::Ptr> ssrc_to_track_;
    std::unordered_map<uint64_t, std::weak_ptr<rtsp::RtpReceiverTrack>> receiver_tick_keys_;
    std::vector<media::EncodedFrame::Ptr> pending_frames_;

    // RTCP 报告与反馈分发，以及端点默认发送出口。
    std::unique_ptr<rtsp::RtcpDispatcher> rtcp_dispatcher_;
    // 下行管理器先于 RTCP 分发器析构，以便安全解除发送器注册。
    endpoint::DownlinkTrackManager downlinks_;
    std::unique_ptr<rtcpx::IRtcpReceiver> rtcp_receiver_;
    std::mutex rtcp_send_mutex_;
    SendRtcpCallback send_rtcp_cb_;
    uint32_t local_rtcp_ssrc_ = 0;
    KeyFrameRequestLimiter unbound_keyframe_requests_;

    EncodedFrameDispatcher frames_;
    endpoint::ReceiveFeedbackHandler receive_feedback_;
};
}

#endif // PACKETIA_MEDIA_SFU_ENDPOINT_H_
