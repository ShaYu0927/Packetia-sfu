#ifndef _TRANSPORT_BWE_CONTROLLER_H_
#define _TRANSPORT_BWE_CONTROLLER_H_

#include <cstdint>
#include <memory>
#include <vector>
#include "NetworkStats.h"
#include "incoming_bitrate_estimator.h"
#include "ou_trend_detect.h"
#include "remote_bitrate_controller.h"

class IBweObserver
{
public:
    virtual ~IBweObserver() = default;
    virtual void OnBweResult(const BweResult& result) = 0;
};

class TransportBweController
{
public:
    BweResult OnTransportFeedback(const media::TransportFeedback& feedback);

    void AddObserver(const std::shared_ptr<IBweObserver>& observer);
    void RemoveObserver(const std::shared_ptr<IBweObserver>& observer);

    double delay_trend_ms() const { return trend_detector_.effective_trend_ms(); }

private:
    void NotifyBweResult(const BweResult& result);

private:
    OUTrendDetect trend_detector_;
    IncomingBitrateEstimator incoming_bitrate_;
    RemoteBitrateController bitrate_controller_;

    std::vector<std::weak_ptr<IBweObserver>> observers_;
};

#endif /* _TRANSPORT_BWE_CONTROLLER_H_ */