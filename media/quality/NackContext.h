#ifndef _NACK_CONTEXT_H_
#define _NACK_CONTEXT_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace media 
{
class NackContext
{
public:
    struct Config
    {
        uint32_t nack_delay_ms = 30;
        uint32_t nack_interval_ms = 50;
        uint32_t max_nack_count = 3;
        uint32_t max_missing_packets = 1000;
        uint32_t max_packet_age_ms = 3000;
        uint16_t max_gap = 100;
    };

    struct Stats
    {
        uint64_t received_packets = 0;
        uint64_t missing_packets = 0;
        uint64_t recovered_packets = 0;
        uint64_t nack_packets = 0;
        uint64_t dropped_missing_packets = 0;
        uint64_t large_gap_resets = 0;
    };

public:
    explicit NackContext(const Config& config);
    void OnRtpPacket(uint16_t seq, uint64_t now_ms);
    std::vector<uint16_t> GetNackSeqs(uint64_t now_ms);

    void Reset();

    const Stats& GetStats() const { return stats_; }

private:
    struct MissingPacket
    {
        uint16_t seq = 0;
        uint64_t first_missing_ms = 0;
        uint64_t last_nack_ms = 0;
        uint32_t nack_count = 0;
    };

private:
    static bool SeqAhead(uint16_t seq, uint16_t base);
    static uint16_t SeqDistance(uint16_t from, uint16_t to);

    void AddMissingRange(uint16_t begin, uint16_t end, uint64_t now_ms);
    void CleanupExpired(uint64_t now_ms);
    void TrimIfNeeded();

private:
    Config config_;
    Stats stats_;

    bool has_highest_seq_ = false;
    uint16_t highest_seq_ = 0;

    std::unordered_map<uint16_t, MissingPacket> missing_;
};
}


#endif /* _NACK_CONTEXT_H_ */