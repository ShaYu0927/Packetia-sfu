#include "RtpRetransmissionCache.h"
#include <utility>

namespace rtsp
{
void RtpRetransmissionCache::Store(uint16_t sequence, const uint8_t* data, size_t size, uint64_t now_ms)
{
    if (!config_.capacity) return;
    auto bytes = common::SharedBuffer::TryCopy(data, size);
    if (bytes.Empty()) return;
    if (packets_.find(sequence) == packets_.end()) order_.push_back(sequence);
    Entry entry;
    entry.packet = std::move(bytes);
    entry.first_send_ms = now_ms;
    packets_[sequence] = std::move(entry);
    while (packets_.size() > config_.capacity && !order_.empty())
    {
        packets_.erase(order_.front());
        order_.pop_front();
    }
}

void RtpRetransmissionCache::Clear()
{
    packets_.clear();
    order_.clear();
}

void RtpRetransmissionCache::Retransmit(const std::vector<uint16_t>& sequences, uint64_t now_ms, const Send& send)
{
    for (const auto sequence : sequences)
    {
        const auto it = packets_.find(sequence);
        if (it == packets_.end()) continue;
        auto& entry = it->second;
        if (now_ms < entry.first_send_ms ||
            (config_.max_age_ms && now_ms - entry.first_send_ms >= config_.max_age_ms) ||
            (config_.max_attempts && entry.retransmit_count >= config_.max_attempts) ||
            (entry.retransmit_count && now_ms >= entry.last_retransmit_ms &&
             now_ms - entry.last_retransmit_ms < config_.min_interval_ms)) continue;
        if (send(sequence, entry.packet))
        {
            entry.last_retransmit_ms = now_ms;
            ++entry.retransmit_count;
        }
    }
}
}
