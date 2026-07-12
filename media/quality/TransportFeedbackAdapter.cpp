#include "TransportFeedbackAdapter.h"

namespace media
{

TransportFeedback TransportFeedbackAdapter::Build(const rtcpx::TransportFeedbackReport& report,
                                                   const PacketHistory& history,
                                                   uint64_t feedback_time_ms) const
{
    TransportFeedback feedback;
    feedback.feedback_time_ms = feedback_time_ms;
    feedback.packet_feedbacks.reserve(report.packets.size());

    for (const auto& item : report.packets)
    {
        PacketSendInfo sent_packet;
        if (!history.GetPacket(item.transport_sequence, sent_packet))
        {
            continue;
        }

        PacketFeedback packet_feedback;
        packet_feedback.sent_packet = sent_packet;
        packet_feedback.received = item.received;
        packet_feedback.receive_time_ms = item.receive_time_ms;
        feedback.packet_feedbacks.push_back(packet_feedback);
    }

    return feedback;
}

}