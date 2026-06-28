#include "NackContext.h"

namespace media 
{

NackContext::NackContext(const Config& config)
    : config_(config)
{
}

bool NackContext::SeqAhead(uint16_t seq, uint16_t base)
{
    // RTP seq 是 uint16_t，会回绕
    // int16_t(seq - base) > 0 表示 seq 在 base 之后
    return static_cast<int16_t>(seq - base) > 0;
}

uint16_t NackContext::SeqDistance(uint16_t from, uint16_t to)
{
    return static_cast<uint16_t>(to - from);
}

void NackContext::OnRtpPacket(uint16_t seq, uint64_t now_ms)
{
    ++stats_.received_packets;

    auto missing_it = missing_.find(seq);
    if (missing_it != missing_.end())
    {
        missing_.erase(missing_it);
        ++stats_.recovered_packets;
    }

    if(!has_highest_seq_)
    {
        highest_seq_ = seq;
        has_highest_seq_ = true;
        return;
    }

    if (SeqAhead(seq, highest_seq_))
    {
        uint16_t expected = static_cast<uint16_t>(highest_seq_ + 1);

        if (seq != expected)
        {
            uint16_t gap = SeqDistance(expected, seq);

            if (gap <= config_.max_gap)
            {
                AddMissingRange(expected, seq, now_ms);
            }
            else
            {
                missing_.clear();
                ++stats_.large_gap_resets;
            }
        }
        highest_seq_ = seq;
    }
    CleanupExpired(now_ms);
    TrimIfNeeded();
}

void NackContext::AddMissingRange(uint16_t begin, uint16_t end, uint64_t now_ms)
{
    for (uint16_t seq = begin; seq != end; seq = static_cast<uint16_t>(seq + 1))
    {
        if (missing_.find(seq) != missing_.end())
        {
            continue;
        }

        MissingPacket pkt;
        pkt.seq = seq;
        pkt.first_missing_ms = now_ms;
        pkt.last_nack_ms = 0;
        pkt.nack_count = 0;

        missing_[seq] = pkt;
        ++stats_.missing_packets;
    }
}

}