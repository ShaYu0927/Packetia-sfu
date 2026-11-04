#include "NetworkStatsCollector.h"
#include "WeakNetController.h"

namespace media
{
namespace
{
void Sample(EwmaFilter& filter, double value, uint64_t now_ms)
{
    const auto last = filter.LastSampleTimeMs();
    if (last && now_ms > *last && now_ms - *last > NetworkStatsCollector::kMaxSampleAgeMs)
        filter.Reset();
    filter.AddSample(value, now_ms);
}

std::optional<double> Value(const EwmaFilter& filter, uint64_t now_ms)
{
    const auto last = filter.LastSampleTimeMs();
    if (!last || now_ms < *last || now_ms - *last > NetworkStatsCollector::kMaxSampleAgeMs)
        return std::nullopt;
    return filter.GetValue(now_ms);
}
}

NetworkStatsCollector::NetworkStatsCollector()
{
    for (auto* filter : {&send_rate_, &recv_rate_, &rtt_, &jitter_, &loss_})
        filter->SetTimeConstantMs(kTimeConstantMs);
}

void NetworkStatsCollector::OnFeedback(const WeakNetFeedback& feedback)
{
    // These legacy fields use zero for an unavailable rate/RTT sample.
    if (feedback.send_bitrate_bps) Sample(send_rate_, feedback.send_bitrate_bps, feedback.now_ms);
    OnTransportFeedback(feedback.receive_bitrate_bps, feedback.loss_rate, feedback.now_ms);
    OnRttSample(feedback.rtt_ms, feedback.now_ms);
    Sample(jitter_, feedback.jitter_ms, feedback.now_ms);
}

void NetworkStatsCollector::OnTransportFeedback(uint32_t recv_bitrate_bps, double loss_rate, uint64_t now_ms)
{
    if (recv_bitrate_bps) Sample(recv_rate_, recv_bitrate_bps, now_ms);
    if (loss_rate >= 0.0 && loss_rate <= 1.0) Sample(loss_, loss_rate, now_ms);
}

void NetworkStatsCollector::OnRttSample(uint32_t rtt_ms, uint64_t now_ms)
{
    if (rtt_ms) Sample(rtt_, rtt_ms, now_ms);
}

SmoothedNetworkMetrics NetworkStatsCollector::GetSnapshot(uint64_t now_ms) const
{
    SmoothedNetworkMetrics result;
    result.send_bitrate_bps = Value(send_rate_, now_ms);
    result.recv_bitrate_bps = Value(recv_rate_, now_ms);
    result.rtt_ms = Value(rtt_, now_ms);
    result.jitter_ms = Value(jitter_, now_ms);
    result.loss_rate = Value(loss_, now_ms);
    return result;
}

void NetworkStatsCollector::Reset()
{
    for (auto* filter : {&send_rate_, &recv_rate_, &rtt_, &jitter_, &loss_}) filter->Reset();
}
}
