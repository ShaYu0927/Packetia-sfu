#include "NackRequester.h"
#include <algorithm>
#include <utility>


namespace rtsp
{

NackRequester::NackRequester(Config config)
    : config_(std::move(config))
{
}

// Register the callback used to send NACK feedback.
void NackRequester::SetNackCallback(NackCallback cb)
{
    nack_callback_ = std::move(cb);
}

// Update the missing packet list according to the received RTP sequence.
void NackRequester::OnReceivedPacket(uint16_t seq, uint64_t now_ms)
{
    if (!initialized_)
    {
        initialized_ = true;
        newest_seq_ = seq;
        return;
    }

    const bool was_missing = missing_packets_.erase(seq) > 0;

    if (was_missing)
    {
        // stats_.recovered_packets++;
        // 后面可以区分乱序恢复还是 NACK 重传恢复。
    }

    if (!IsNewerSequenceNumber(seq, newest_seq_))
    {
        return;
    }

    const uint16_t gap = static_cast<uint16_t>(seq - newest_seq_);
    if (gap > 1)
    {
        AddMissingPackets(static_cast<uint16_t>(newest_seq_ + 1), seq, now_ms);
    }

    newest_seq_ = seq;
}

// Check pending missing packets and generate NACK requests when needed.
void NackRequester::Process(uint64_t now_ms)
{
    if (!initialized_ || missing_packets_.empty())
    {
        return;
    }

    std::vector<uint16_t> nack_batch;
    nack_batch.reserve(std::min(missing_packets_.size(), config_.max_batch_size));

    for (auto it = missing_packets_.begin(); it != missing_packets_.end();)
    {
        const uint16_t seq = it->first;
        MissingPacket& packet = it->second;

        if (now_ms < packet.first_missing_ms)
        {
            it = missing_packets_.erase(it);
            continue;
        }
        const uint64_t packet_age_ms = now_ms - packet.first_missing_ms;

        /* Abandoned */
        if (packet_age_ms >= config_.max_packet_age_ms || packet.nack_count >= config_.max_retries)
        {
            it = missing_packets_.erase(it);
            continue;
        }

        /*
         * 状态：WaitingReorder
         *
         * 刚发现序号缺口时不能立即认为丢包，
         * 先给乱序 RTP 留出到达时间。
         */
        if (packet_age_ms < config_.reorder_wait_ms)
        {
            ++it;
            continue;
        }

        bool should_send_nack = false;
        if (packet.nack_count == 0)
        {
            should_send_nack = true;
        }
        else
        {
            if (now_ms >= packet.last_nack_ms)
            {
                const uint64_t elapsed_since_last_nack = now_ms - packet.last_nack_ms;
                should_send_nack = elapsed_since_last_nack >= config_.retry_interval_ms;
            }
        }

        if (should_send_nack)
        {
            nack_batch.push_back(seq);
            packet.last_nack_ms = now_ms;
            ++packet.nack_count;

            ++it;

            if (nack_batch.size() >= config_.max_batch_size)
            {
                break;
            }
            continue;
        }
        ++it;

    }

    if (!nack_batch.empty() && nack_callback_)
    {
        nack_callback_(nack_batch);
    }
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
