#include "RtcpBweBridge.h"
#include <chrono>

namespace media
{
namespace
{
uint64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}


RtcpBweBridge::RtcpBweBridge(TransportBweController* controller)
    : controller_(controller)
{
}

void RtcpBweBridge::SetController(TransportBweController* controller)
{
    controller_ = controller;
}

void RtcpBweBridge::OnPacketSent(const PacketSendInfo& packet)
{
    packet_history_.AddPacket(packet);
}

BweResult RtcpBweBridge::OnTransportFeedbackReport(const rtcpx::TransportFeedbackReport& report)
{
    BweResult result;
    if (!controller_)
    {
        return result;
    }

    TransportFeedback feedback = adapter_.Build(report, packet_history_, NowMs());
    if (feedback.packet_feedbacks.empty())
    {
        return result;
    }

    return controller_->OnTransportFeedback(feedback);
}

}