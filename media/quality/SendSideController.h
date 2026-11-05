#ifndef PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_

#include "GoogCcNetworkController.h"
#include "PacketHistory.h"
#include "TransportFeedbackAdapter.h"
#include "TransportSequenceAllocator.h"
#include "DownlinkBandwidthAllocator.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace media
{
// One instance per downstream transport. Serializes packet history and GCC
// across audio/video workers and the transport's RTCP receive thread.
class SendSideController
{
public:
    using UpdateCallback = std::function<void(const NetworkControlUpdate&)>;

    explicit SendSideController(const NetworkControllerConfig& config = {});
    std::shared_ptr<TransportSequenceAllocator> SequenceAllocator() const;
    void OnPacketSent(const PacketSendInfo& packet);
    void RegisterSender(uint32_t ssrc, uint32_t clock_rate);
    void UnregisterSender(uint32_t ssrc);
    bool OnRtcpPacket(const uint8_t* data, size_t size, uint64_t receive_time_ms);
    void OnReceiverFeedback(const WeakNetFeedback& feedback);
    // Driven by the owning media worker even when RTP/RTCP stops arriving.
    // Like route changes, a timeout publishes one empty update to clear rates.
    void OnProcessInterval(uint64_t now_ms);
    void SetNetworkAvailable(bool available);
    // Usable new ICE path, even if availability remained true (NAT rebind).
    // Clear old feedback/estimate while preserving sender and TWCC identity.
    void OnNetworkRouteChanged();
    void SetUpdateCallback(UpdateCallback callback);
    NetworkControlUpdate GetNetworkState() const;
    // Independent listeners avoid replacing the application's update callback.
    // Notifications run outside mutex_; listeners should only schedule work.
    using BudgetObserverId = uint64_t;
    BudgetObserverId AddBudgetObserver(std::function<void()> observer);
    void RemoveBudgetObserver(BudgetObserverId id);
    DownlinkBandwidthBudget GetBandwidthBudget() const;
    ProbeDecision GetProbeDecision(const ProbeRequest& request);
    std::optional<ProbePlan> StartProbe(ProbePlan plan, uint64_t now_ms, uint64_t path_epoch);
    void OnProbeEvent(const ProbeEvent& event);

private:
    void NotifyBudgetObservers();
    bool ProcessIntervalLocked(uint64_t now_ms);
    bool IsTwccRecent(uint64_t now_ms) const
    {
        if (!has_twcc_)
            return false;

        if (now_ms < last_valid_twcc_receive_time_ms_)
            return true;

        return now_ms - last_valid_twcc_receive_time_ms_ < 1000;
    }

private:
    mutable std::mutex mutex_;
    NetworkControllerConfig config_;
    GoogCcNetworkController controller_;
    std::shared_ptr<TransportSequenceAllocator> sequence_allocator_;
    PacketHistory history_;
    TransportFeedbackAdapter adapter_;
    bool available_;
    bool has_twcc_ = false;
    uint64_t last_valid_twcc_receive_time_ms_ = 0;
    UpdateCallback update_callback_;
    uint64_t path_epoch_ = 1;
    int next_probe_cluster_id_ = 0;
    BudgetObserverId next_observer_id_ = 1;
    std::unordered_map<BudgetObserverId, std::function<void()>> budget_observers_;
    std::unordered_map<uint32_t, uint32_t> sender_clock_rates_;
};
}

#endif // PACKETIA_MEDIA_QUALITY_SENDSIDECONTROLLER_H_
