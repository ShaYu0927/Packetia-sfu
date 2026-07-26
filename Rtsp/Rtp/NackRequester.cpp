#include "NackRequester.h"


namespace rtsp
{

// Register the callback used to send NACK feedback.
void NackRequester::SetNackCallback(NackCallback cb)
{
    nack_callback_ = std::move(cb);
}

// Update the missing packet list according to the received RTP sequence.
void NackRequester::OnReceivedPacket(uint16_t seq, uint64_t now_ms)
{
    if(!initialized_)
    {
        initialized_ = true;
        newest_seq_ = seq;
        return;
    }

    missing_packets_.erase(seq);

    if(!IsNewerSequenceNumber(seq, newest_seq_))
    {
        return;
    }

    const uint16_t gap = static_cast<uint16_t>(seq - newest_seq_);
    if(gap > 1)
    {
        AddMissingPackets(static_cast<uint16_t>(newest_seq_ + 1), seq, now_ms);
    }
    newest_seq_ = seq;
}

// Check pending missing packets and generate NACK requests when needed.
void NackRequester::Process(uint64_t now_ms)
{
    
}

// Reset all runtime states and clear the pending NACK list.
void NackRequester::Reset()
{
    initialized_ = 0;
    newest_seq_  = 0;
    missing_packets_.clear();
}

void NackRequester::AddMissingPackets(uint16_t begin, uint16_t end, uint64_t now_ms)
{
    uint16_t seq = begin;
    while(seq != end)
    {
        if(missing_packets_.size() >= config_.max_nack_list_size)
        {
            break;
        }

        // Ignore duplicate entries.
        missing_packets_.try_emplace(
            seq,
            MissingPacket{
                .first_missing_ms = now_ms,
                .last_nack_ms = 0,
                .nack_count = 0
            });

        ++seq;
    }
}

}