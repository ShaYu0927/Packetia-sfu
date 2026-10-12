#include "transport_bwe_controller.h"
#include <algorithm>
#include <limits>

void TransportBweController::SetBitrateConstraints(const media::BitrateConstraints& constraints)
{
    bitrate_controller_.SetBitrateConstraints(constraints.min_bitrate_bps,
        constraints.start_bitrate_bps, constraints.max_bitrate_bps);
    Reset();
}

void TransportBweController::SetRtt(uint32_t rtt_ms)
{
    if (!rtt_ms) return;
    rtt_ms_ = rtt_ms;
    bitrate_controller_.SetRtt(rtt_ms);
}

void TransportBweController::Reset()
{
    trend_detector_.Reset();
    incoming_bitrate_.Reset();
    bitrate_controller_.Reset();
    rtt_ms_ = 0;
}

BweResult TransportBweController::OnTransportFeedback(const media::TransportFeedback& feedback)
{
    trend_detector_.SetNetworkEstimate(bitrate_controller_.target_bitrate_bps(), rtt_ms_);
    trend_detector_.SetRateControlRegion(bitrate_controller_.region());
    BandwidthUsage usage = BandwidthUsage::kNormal;
    uint32_t incoming_bitrate_bps = 0;

    std::vector<const media::PacketFeedback*> received_packets;
    received_packets.reserve(feedback.packet_feedbacks.size());

    for (const auto& packet : feedback.packet_feedbacks)
    {
        if (packet.received)
        {
            received_packets.push_back(&packet);
        }
    }

    std::sort(received_packets.begin(), received_packets.end(),
              [](const media::PacketFeedback* lhs, const media::PacketFeedback* rhs) {
                  const auto& left = lhs->sent_packet;
                  const auto& right = rhs->sent_packet;

                  if (left.transport_sequence >= 0 && right.transport_sequence >= 0 &&
                      left.transport_sequence != right.transport_sequence)
                  {
                      return left.transport_sequence < right.transport_sequence;
                  }

                  return left.send_time_ms < right.send_time_ms;
              });

    for (const auto* packet : received_packets)
    {
        const auto& sent = packet->sent_packet;
        usage = trend_detector_.OnPacket(static_cast<uint32_t>(sent.send_time_ms),
                                         static_cast<int64_t>(packet->receive_time_ms),
                                         sent.size_bytes);
    }

    // Receive-rate samples need receive-clock order; the trend detector above
    // uses send order and skips backwards arrival samples caused by reordering.
    std::sort(received_packets.begin(), received_packets.end(), [](const auto* a, const auto* b) {
        return a->receive_time_ms < b->receive_time_ms;
    });
    for (const auto* packet : received_packets)
        incoming_bitrate_bps = incoming_bitrate_.Update(packet->sent_packet.size_bytes,
            static_cast<int64_t>(packet->receive_time_ms));

    BweResult result = bitrate_controller_.Update(usage,
                                                  incoming_bitrate_bps,
                                                  feedback.LossRate(),
                                                  static_cast<int64_t>(std::min<uint64_t>(feedback.feedback_time_ms,
                                                      std::numeric_limits<int64_t>::max())));
    result.delay_trend_ms = trend_detector_.effective_trend_ms();
    NotifyBweResult(result);
    return result;
}

void TransportBweController::AddObserver(const std::shared_ptr<IBweObserver>& observer)
{
    if (!observer)
    {
        return;
    }

    observers_.push_back(observer);
}

void TransportBweController::RemoveObserver(const std::shared_ptr<IBweObserver>& observer)
{
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [&](const std::weak_ptr<IBweObserver>& weak_observer) {
                           auto locked = weak_observer.lock();
                           return !locked || locked == observer;
                       }),
        observers_.end());
}

void TransportBweController::NotifyBweResult(const BweResult& result)
{
    std::vector<std::shared_ptr<IBweObserver>> snapshot;
    snapshot.reserve(observers_.size());

    for (auto& observer : observers_)
    {
        if (auto locked = observer.lock())
        {
            snapshot.push_back(locked);
        }
    }

    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [](const std::weak_ptr<IBweObserver>& observer) {
                           return observer.expired();
                       }),
        observers_.end());

    for (auto& observer : snapshot)
    {
        observer->OnBweResult(result);
    }
}
