#ifndef _NETWORK_CONTROLLER_H_
#define _NETWORK_CONTROLLER_H_

#include <memory>
#include <vector>
#include "WeakNetController.h"
#include "bwe/transport_bwe_controller.h"

namespace media
{

class INetworkControlObserver
{
public:
    virtual ~INetworkControlObserver() = default;
    virtual void OnNetworkControlUpdate(const NetworkControlUpdate& update) = 0;
};

class NetworkController : public IBweObserver
{
public:
    NetworkController();

    void SetBitrateConstraints(const BitrateConstraints& constraints);

    NetworkControlUpdate OnFeedback(const WeakNetFeedback& feedback);
    NetworkControlUpdate OnBweResultAndFeedback(const BweResult& bwe, const WeakNetFeedback& feedback);

    void OnBweResult(const BweResult& result) override;

    void AddObserver(const std::shared_ptr<INetworkControlObserver>& observer);
    void RemoveObserver(const std::shared_ptr<INetworkControlObserver>& observer);

    NetworkStatsSnapshot GetSnapshot() const { return snapshot_; }

private:
    NetworkQualityLevel EstimateQuality(const WeakNetFeedback& feedback) const;
    NetworkControlUpdate BuildUpdate(const BweResult* bwe, const WeakNetFeedback& feedback);
    bool ShouldRequestKeyFrame(uint64_t now_ms, NetworkQualityLevel quality);
    void NotifyNetworkControlUpdate(const NetworkControlUpdate& update);

private:
    BitrateConstraints bitrate_;
    WeakNetFeedback latest_feedback_;
    bool has_feedback_ = false;

    uint32_t fallback_target_bitrate_bps_ = 800 * 1000;
    uint32_t stable_target_bitrate_bps_ = 800 * 1000;

    uint64_t last_keyframe_request_ms_ = 0;
    uint64_t keyframe_interval_ms_ = 1000;

    NetworkStatsSnapshot snapshot_;
    std::vector<std::weak_ptr<INetworkControlObserver>> observers_;
};

}

#endif /* _NETWORK_CONTROLLER_H_ */