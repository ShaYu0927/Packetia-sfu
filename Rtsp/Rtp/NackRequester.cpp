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

void NackRequester::SetRecoveryFailureCallback(RecoveryFailureCallback cb)
{
    recovery_failure_callback_ = std::move(cb);
}

// Update the missing packet list according to the received RTP sequence.
NackRequester::PacketResult NackRequester::OnReceivedPacket(uint16_t seq, uint64_t now_ms, bool recovered_by_fec)
{
    PacketResult result;
    if (!initialized_)
    {
        initialized_ = true;
        newest_seq_ = seq;
        return result;
    }

    auto missing = missing_packets_.find(seq);
    if (missing != missing_packets_.end())
    {
        result.was_missing = true;
        result.times_nacked = missing->second.nack_count;
        result.recovered_after_nack = result.times_nacked > 0 || recovered_by_fec;
        if (now_ms >= missing->second.first_missing_ms)
        {
            result.recovery_time_ms = now_ms - missing->second.first_missing_ms;
        }
        if (result.recovered_after_nack)
        {
            ++stats_.recovered_packets;
        }
        else
        {
            ++stats_.reordered_packets;
        }
        missing_packets_.erase(missing);
    }

    if (!IsNewerSequenceNumber(seq, newest_seq_))
    {
        return result;
    }

    const uint16_t gap = static_cast<uint16_t>(seq - newest_seq_);
    if (gap > config_.max_sequence_gap)
    {
        missing_packets_.clear();
        newest_seq_ = seq;
        ++stats_.large_gap_resets;
        return result;
    }
    if (gap > 1)
    {
        AddMissingPackets(static_cast<uint16_t>(newest_seq_ + 1), seq, now_ms);
    }

    newest_seq_ = seq;
    return result;
}

// Check pending missing packets and generate NACK requests when needed.
void NackRequester::Process(uint64_t now_ms)
{
    if (!initialized_ || missing_packets_.empty())
    {
        return;
    }

    std::vector<uint16_t> nack_batch;
    std::vector<uint16_t> abandoned_batch;
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
            abandoned_batch.push_back(seq);
            ++stats_.abandoned_packets;
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
                should_send_nack = elapsed_since_last_nack >= RetryIntervalMs();
            }
        }

        if (should_send_nack)
        {
            nack_batch.push_back(seq);
            packet.last_nack_ms = now_ms;
            ++packet.nack_count;
            ++stats_.nack_requests;

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

    if (!abandoned_batch.empty() && recovery_failure_callback_ &&
        (last_failure_callback_ms_ == 0 || now_ms < last_failure_callback_ms_ ||
         now_ms - last_failure_callback_ms_ >= config_.failure_callback_interval_ms))
    {
        last_failure_callback_ms_ = now_ms;
        recovery_failure_callback_(abandoned_batch);
    }
}

void NackRequester::UpdateRtt(uint32_t rtt_ms)
{
    rtt_ms_ = rtt_ms;
}

void NackRequester::ClearUpTo(uint16_t seq)
{
    for (auto it = missing_packets_.begin(); it != missing_packets_.end();)
    {
        if (!IsNewerSequenceNumber(it->first, seq))
        {
            it = missing_packets_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// Reset all runtime states and clear the pending NACK list.
void NackRequester::Reset()
{
    initialized_ = 0;
    newest_seq_  = 0;
    rtt_ms_ = 0;
    last_failure_callback_ms_ = 0;
    missing_packets_.clear();
    stats_ = Stats{};
}

uint32_t NackRequester::RetryIntervalMs() const
{
    const uint32_t interval = rtt_ms_ == 0 ? config_.retry_interval_ms : rtt_ms_;
    return std::clamp(interval, config_.min_retry_interval_ms, config_.max_retry_interval_ms);
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
        MissingPacket packet;
        packet.first_missing_ms = now_ms;
        const auto [it, inserted] = missing_packets_.try_emplace(seq, packet);
        (void)it;
        if (inserted)
        {
            ++stats_.missing_packets;
        }

        ++seq;
    }
}

}
