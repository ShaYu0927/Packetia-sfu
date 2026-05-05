#include "RtpSender.h"

namespace room
{
RtpSender::RtpSender(std::shared_ptr<ITransport> transport)
    : transport_(std::move(transport)),
      packet_history_(RtpPacketHistory::Config{}) 
{
}


bool RtpSender::SendRtpPacket(const uint8_t* data, size_t size, uint64_t now_ms) 
{
    if (!transport_ || !data || size < 12) 
    {
        return false;
    }

    bool ok = transport_->Send(data, size);
    if (!ok) 
    {
        return false;
    }

    /* 缓存 用于NACK */
    packet_history_.PutRtpPacket(data, size, now_ms);

    return true;
}

void RtpSender::Retransmit(uint16_t sequence_number, uint64_t now_ms) 
{
    auto packet = packet_history_.Get(sequence_number);
    if (!packet) 
    {
        return;
    }

    if (!transport_) 
    {
        return;
    }

    /* 重发NACK */
    bool ok = transport_->Send(packet->data.data(), packet->data.size());
    if (ok) 
    {
        packet_history_.MarkRetransmitted(sequence_number, now_ms);
    }
}

void RtpSender::OnNack(const std::vector<uint16_t>& lost_sequences, uint64_t now_ms) 
{
    for (uint16_t seq : lost_sequences) 
    {
        Retransmit(seq, now_ms);
    }
}

RtpPacketHistory& RtpSender::PacketHistory() 
{
    return packet_history_;
}



}