#ifndef PACKETIA_MEDIA_QUALITY_BWE_TRANSPORT_BWE_CONTROLLER_H_
#define PACKETIA_MEDIA_QUALITY_BWE_TRANSPORT_BWE_CONTROLLER_H_

#include <cstdint>
#include <memory>
#include <vector>
#include "NetworkStats.h"
#include "IncomingBitrateEstimator.h"
#include "OUTrendDetect.h"
#include "RemoteBitrateController.h"
#include "PacketGroupObserver.h"

class IBweObserver
{
public:
    virtual ~IBweObserver() = default;
    virtual void OnBweResult(const BweResult& result) = 0;
};

class TransportBweController
{
public:
    explicit TransportBweController(std::unique_ptr<media::IPacketGroupObserver> groups = {});
    BweResult OnTransportFeedback(const media::TransportFeedback& feedback);
    void SetBitrateConstraints(const media::BitrateConstraints& constraints);
    void SetRtt(uint32_t rtt_ms);
    void Reset();

    void AddObserver(const std::shared_ptr<IBweObserver>& observer);
    void RemoveObserver(const std::shared_ptr<IBweObserver>& observer);

    double delay_trend_ms() const { return last_delay_trend_ms_; }
    const media::PacketGroupObservation& GroupObservation() const { return group_observation_; }

private:
    void NotifyBweResult(const BweResult& result);

private:
    OUTrendDetect trend_detector_;
    IncomingBitrateEstimator incoming_bitrate_;
    RemoteBitrateController bitrate_controller_;
    uint32_t rtt_ms_ = 0;
    std::unique_ptr<media::IPacketGroupObserver> group_observer_;
    media::PacketGroupObservation group_observation_;
    double last_delay_trend_ms_ = 0.0;

    std::vector<std::weak_ptr<IBweObserver>> observers_;
};

#endif /* PACKETIA_MEDIA_QUALITY_BWE_TRANSPORT_BWE_CONTROLLER_H_ */
