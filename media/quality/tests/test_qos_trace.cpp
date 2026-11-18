#include <gtest/gtest.h>
#include "QosTrace.h"
#include "SendSideController.h"
#include "third/nlohmann/json.hpp"

namespace
{
using Json = nlohmann::json;
using namespace media;
struct Recorder
{
    std::vector<Json> records;
    QosTrace trace{[this](const char* event, const packetia::logging::LogFields& fields) {
        records.push_back(Json::parse(packetia::logging::FormatRecord(LogFormat::Json,
            "test", "node", __FILE__, __LINE__, 2, "test", event, fields)));
    }};
    std::vector<Json> Events(const char* event) const
    {
        std::vector<Json> result;
        for (const auto& record : records) if (record.at("event") == event) result.push_back(record.at("data"));
        return result;
    }
};

TEST(QosTrace, SamplingUsesCompletionsAndMonotonicWindowsAndBoundsLogFrequency)
{
    Recorder recorder;
    TransportDiagnostics sample;
    sample.at_time_ms = 1000; sample.path_epoch = 1; sample.network_available = true;
    transport::PacedSendQueue::Stats queue;
    queue.queued_bytes = 2400; queue.queued_packets = 2; queue.oldest_wait_ms = 12;
    queue.pacing_bitrate_bps = 800000; queue.estimated_drain_ms = 24;
    recorder.trace.Observe(42, sample, &queue);
    EXPECT_TRUE(recorder.Events("qos_sample").front().at("sent_rtp_bps").is_null());
    sample.at_time_ms = 1999; sample.sent_rtp_bytes_total = 100000; sample.sent_packets_total = 100;
    recorder.trace.Observe(42, sample);
    EXPECT_EQ(recorder.Events("qos_sample").size(), 1U);
    sample.at_time_ms = 2000;
    recorder.trace.Observe(42, sample, &queue);
    const auto measured = recorder.Events("qos_sample").back();
    EXPECT_EQ(measured.at("sent_rtp_bps"), 800000);
    EXPECT_EQ(measured.at("sent_packets_per_second"), 100);
    EXPECT_EQ(measured.at("transport_id"), 42);
    EXPECT_EQ(measured.at("estimated_drain_ms"), 24);
    EXPECT_EQ(measured.at("oldest_wait_ms"), 12);
    EXPECT_TRUE(measured.at("loss_rate").is_null());
}

TEST(QosTrace, PathChangesAndClockRollbackCannotCreateFakeThroughput)
{
    Recorder recorder;
    TransportDiagnostics sample;
    sample.at_time_ms = 1000; sample.path_epoch = 1; sample.sent_rtp_bytes_total = 10000;
    recorder.trace.Observe(42, sample);
    sample.at_time_ms = 2000; sample.path_epoch = 2; sample.sent_rtp_bytes_total = 20000;
    recorder.trace.Observe(42, sample);
    EXPECT_TRUE(recorder.Events("qos_sample").back().at("sent_rtp_bps").is_null());
    sample.at_time_ms = 3000; sample.sent_rtp_bytes_total = 30000;
    recorder.trace.Observe(42, sample);
    EXPECT_EQ(recorder.Events("qos_sample").back().at("sent_rtp_bps"), 80000);
    sample.at_time_ms = 2500;
    recorder.trace.Observe(42, sample);
    EXPECT_TRUE(recorder.Events("qos_sample").back().at("sent_rtp_bps").is_null());
    EXPECT_TRUE(recorder.Events("qos_sample").back().at("queued_bytes").is_null());
}

TEST(QosTrace, StateAndProbeResultEventsAreNotRepeatedOnEachTick)
{
    Recorder recorder;
    TransportDiagnostics sample;
    sample.at_time_ms = 1000; sample.path_epoch = 1;
    recorder.trace.Observe(1, sample);
    sample.at_time_ms = 1020; sample.controller.feedback_valid = true;
    sample.controller.feedback_source = "twcc";
    sample.controller.usage = BandwidthUsage::kOverUsing;
    sample.last_probe_result = ProbeResult{4, 1, ProbeOutcome::Congested, 0, 1010};
    recorder.trace.Observe(1, sample);
    sample.at_time_ms = 1040;
    recorder.trace.Observe(1, sample);
    EXPECT_EQ(recorder.Events("qos_control_state").size(), 2U);
    EXPECT_EQ(recorder.Events("qos_control_state").back().at("usage"), "overusing");
    EXPECT_EQ(recorder.Events("qos_probe_result").size(), 1U);
    EXPECT_TRUE(recorder.Events("qos_probe_result").back().at("capacity_bps").is_null());
    EXPECT_EQ(recorder.Events("qos_sample").size(), 1U);
    sample.last_probe_result = ProbeResult{4, 2, ProbeOutcome::Succeeded, 900000, 1050};
    recorder.trace.Observe(1, sample);
    EXPECT_EQ(recorder.Events("qos_probe_result").size(), 2U);
    EXPECT_EQ(recorder.Events("qos_probe_result").back().at("capacity_bps"), 900000);
}

TEST(QosDiagnostics, ReadOnlyQueriesKeepExpiredFeedbackUnknownAndPreserveControllerState)
{
    SendSideController controller;
    WeakNetFeedback feedback;
    feedback.now_ms = 1000; feedback.rtt_ms = 80; feedback.jitter_ms = 4; feedback.loss_rate = 0.1;
    controller.OnReceiverFeedback(feedback);
    const auto before = controller.GetBandwidthBudget();
    const auto fresh = controller.GetDiagnostics(1000);
    ASSERT_TRUE(fresh.controller.feedback_valid);
    EXPECT_STREQ(fresh.controller.feedback_source, "rr");
    EXPECT_DOUBLE_EQ(*fresh.controller.loss_rate, 0.1);
    EXPECT_DOUBLE_EQ(*fresh.controller.rtt_ms, 80);
    EXPECT_FALSE(fresh.controller.delay_trend_ms);
    const auto stale = controller.GetDiagnostics(6000);
    EXPECT_FALSE(stale.controller.feedback_valid);
    EXPECT_FALSE(stale.controller.loss_rate);
    EXPECT_FALSE(stale.controller.control.has_target_rate);
    EXPECT_FALSE(controller.GetDiagnostics(900).controller.feedback_valid);
    EXPECT_EQ(controller.GetBandwidthBudget().target_bps, before.target_bps);
}

TEST(QosDiagnostics, LateCompletionsAreExcludedAndLifetimeCountersSurviveRouteChanges)
{
    SendSideController controller;
    PacketSendInfo packet;
    packet.transport_sequence = -1; packet.size_bytes = 1200;
    const auto epoch = controller.PathEpoch();
    EXPECT_TRUE(controller.OnPacketSentForPath(packet, epoch));
    EXPECT_EQ(controller.GetDiagnostics(1000).sent_rtp_bytes_total, 1200U);
    controller.OnNetworkRouteChanged();
    EXPECT_FALSE(controller.OnPacketSentForPath(packet, epoch));
    packet.is_probe = true;
    EXPECT_TRUE(controller.OnPacketSentForPath(packet, controller.PathEpoch()));
    const auto snapshot = controller.GetDiagnostics(2000);
    EXPECT_EQ(snapshot.sent_packets_total, 2U);
    EXPECT_EQ(snapshot.sent_rtp_bytes_total, 2400U);
    EXPECT_EQ(snapshot.sent_probe_bytes_total, 1200U);
    EXPECT_FALSE(snapshot.controller.feedback_valid);
}
}
