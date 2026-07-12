#ifndef _RTCP_BWE_BRIDGE_H_
#define _RTCP_BWE_BRIDGE_H_

#include "PacketHistory.h"
#include "TransportFeedbackAdapter.h"
#include "bwe/transport_bwe_controller.h"

namespace media
{

class RtcpBweBridge
{
public:
    explicit RtcpBweBridge(TransportBweController* controller = nullptr);

    void SetController(TransportBweController* controller);

    void OnPacketSent(const PacketSendInfo& packet);
    BweResult OnTransportFeedbackReport(const rtcpx::TransportFeedbackReport& report);

    PacketHistory& packet_history() { return packet_history_; }
    const PacketHistory& packet_history() const { return packet_history_; }

private:
    PacketHistory packet_history_;
    TransportFeedbackAdapter adapter_;
    TransportBweController* controller_ = nullptr;
};

}

#endif /* _RTCP_BWE_BRIDGE_H_ */