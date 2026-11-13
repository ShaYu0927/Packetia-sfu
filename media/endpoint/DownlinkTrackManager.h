#ifndef PACKETIA_MEDIA_ENDPOINT_DOWNLINK_TRACK_MANAGER_H_
#define PACKETIA_MEDIA_ENDPOINT_DOWNLINK_TRACK_MANAGER_H_

#include "SfuEndpointTypes.h"
#include "SendSideController.h"

#include <unordered_map>

namespace rtsp { class RtcpDispatcher; }

namespace media::endpoint
{
class DownlinkTrackManager
{
public:
    DownlinkTrackManager() = default;
    DownlinkTrackManager(const DownlinkTrackManager&) = delete;
    DownlinkTrackManager& operator=(const DownlinkTrackManager&) = delete;

    struct Callbacks
    {
        std::function<void()> source_closed;
        rtsp::RtpSenderTrack::KeyFrameRequestCallback request_keyframe;
    };

    ~DownlinkTrackManager();
    void SetDispatcher(rtsp::RtcpDispatcher* dispatcher);
    bool Configure(const std::string& id, const rtsp::RtpSenderTrackConfig& config, std::shared_ptr<IMediaTransport> transport, CodecId codec);
    
    bool Subscribe(const std::string& id, const ::TrackInfo& source_info, MediaTrackRef ref, const std::shared_ptr<Subscription>& route, Callbacks callbacks);
    bool Unsubscribe(const std::string& id);
    void Prune();
    void Clear();
    size_t Count() const;
    void Deliver(const std::shared_ptr<Subscription>& route, common::BufferView packet);
    void SetSchedule(std::function<void()> schedule) { schedule_ = std::move(schedule); }
    void Tick(uint64_t now_ms, bool force = false);
    bool SetPolicy(const std::string& id, const DownlinkSubscriptionPolicy& policy);
    bool SetAudioMuted(const std::string& id, bool muted, bool& changed);
    std::optional<DownlinkBandwidthStatus> GetStatus(const std::string& id) const;

private:
    rtsp::RtcpDispatcher* dispatcher_ = nullptr;
    std::unordered_map<std::string, SubscriptionParameters> parameters_;
    std::unordered_map<std::string, std::shared_ptr<Subscription>> subscriptions_;
    struct TransportGroup
    {
        std::shared_ptr<SendSideController> controller;
        SendSideController::BudgetObserverId observer = 0;
        DownlinkBandwidthAllocator allocator;
        uint64_t path_epoch = 0;
        std::optional<uint64_t> last_probe_ms;
        std::optional<uint64_t> probe_started_ms;
        std::string probe_track;
        std::optional<ProbePlan> probe_plan;
        uint64_t probe_generation = 0;
        uint64_t probe_bytes = 0;
    };
    std::unordered_map<uint64_t, TransportGroup> transports_;
    std::function<void()> schedule_;
    uint64_t next_tick_ms_ = 0;
    uint32_t RequiredBitrate(const Subscription& route) const;
    void Probe(TransportGroup& group, const DownlinkBandwidthBudget& budget,
        const std::vector<DownlinkDemand>& demands, uint64_t now_ms);
    void FinishProbe(TransportGroup& group, uint64_t now_ms, ProbeEventType type);
};
}
#endif
