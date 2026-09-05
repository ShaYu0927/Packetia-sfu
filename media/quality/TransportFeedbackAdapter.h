#ifndef _TRANSPORT_FEEDBACK_ADAPTER_H_
#define _TRANSPORT_FEEDBACK_ADAPTER_H_

#include "NetworkStats.h"
#include "PacketHistory.h"
#include "RtcpContext.h"
#include "RtcpNack.h"
#include <optional>
#include <map>

namespace media
{

struct TransPacketHistory
{
    int64_t sequence_number;        /* Transport sequence number */
    uint16_t rtp_sequence_number;   /* RTP sequence number */

    uint64_t send_time_ms;          /* Packet send time in milliseconds */
    size_t size;                    /* Packet size in bytes */

    uint32_t ssrc;                 /* RTP synchronization source identifier */

    bool retransmission;           /* Whether this is a retransmitted packet */
    bool reported_lost;            /* Whether this packet has been reported lost */
};

struct TransportPacketsFeedback
{
    uint64_t feedback_time_ms = 0;        // Feedback receive time
    uint64_t data_in_flight_bytes = 0;    // Unacknowledged bytes in flight

    std::vector<TransPacketHistory> packet_feedbacks;
};

class TransportFeedbackAdapter
{
public:
    void AddPacket(const TransPacketHistory& packet);

    void OnPacketSend(int64_t transport_sequence_number, uint64_t send_time_ms);

    TransportPacketsFeedback ProcessFeedback(const rtcpx::RtcpTransportFeedback& feedback, uint64_t feedback_time_ms);

    std::optional<PacketHistory> RetrievePacket(int64_t transport_sequence_number, bool received);


    TransportFeedback Build(const rtcpx::TransportFeedbackReport& report, const PacketHistory& history, uint64_t feedback_time_ms) const;

private:
    std::map<int64_t, TransPacketHistory> history_;

    uint64_t data_in_flight_bytes_ = 0;
    int64_t last_ack_sequence_number_ = -1;
};

}

#endif /* _TRANSPORT_FEEDBACK_ADAPTER_H_ */