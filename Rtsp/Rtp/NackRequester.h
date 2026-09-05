#ifndef _NACK_REQUESTER_H_
#define _NACK_REQUESTER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace rtsp 
{
class NackRequester
{
public:
    struct Config
    {
        uint32_t reorder_wait_ms        = 20;               // Wait time before treating a missing packet as lost.
        uint32_t retry_interval_ms      = 50;               // Minimum interval between NACK retries.
        uint32_t max_packet_age_ms      = 500;              // Maximum time to keep a missing packet.
        uint32_t max_retries            = 5;                // Maximum NACK retries per packet.
        std::size_t max_nack_list_size  = 1000;             // Maximum number of tracked missing packets.
        std::size_t max_batch_size      = 50;               // Maximum number of sequence numbers per NACK batch.
        uint16_t max_sequence_gap       = 100;              // Reset instead of NACKing an implausibly large jump.
        uint32_t min_retry_interval_ms  = 20;
        uint32_t max_retry_interval_ms  = 200;
        uint32_t failure_callback_interval_ms = 500;        // Rate-limit recovery failure notifications.
    };

    struct PacketResult
    {
        bool was_missing = false;
        bool recovered_after_nack = false;
        uint32_t times_nacked = 0;
        uint64_t recovery_time_ms = 0;
    };

    struct Stats
    {
        uint64_t missing_packets = 0;
        uint64_t reordered_packets = 0;
        uint64_t recovered_packets = 0;
        uint64_t nack_requests = 0;
        uint64_t abandoned_packets = 0;
        uint64_t large_gap_resets = 0;
    };

    using NackCallback = std::function<void(const std::vector<uint16_t>&)>;
    using RecoveryFailureCallback = std::function<void(const std::vector<uint16_t>&)>;

public:
    explicit NackRequester(Config config);

    void SetNackCallback(NackCallback cb);
    void SetRecoveryFailureCallback(RecoveryFailureCallback cb);

    PacketResult OnReceivedPacket(uint16_t seq, uint64_t now_ms, bool recovered_by_fec = false);

    void Process(uint64_t now_ms);

    void UpdateRtt(uint32_t rtt_ms);

    void ClearUpTo(uint16_t seq);

    void Reset();

    const Stats& GetStats() const { return stats_; }

private:
    struct MissingPacket
    {
        uint64_t first_missing_ms = 0;
        uint64_t last_nack_ms = 0;
        uint32_t nack_count = 0;
    };

    bool IsNewerSequenceNumber(uint16_t seq, uint16_t reference)
    {
        return seq != reference && static_cast<uint16_t>(seq - reference) < 0x8000;
    }

    void AddMissingPackets(uint16_t begin, uint16_t end, uint64_t now_ms);
    uint32_t RetryIntervalMs() const;

private:
    Config config_;

    bool initialized_ = false;
    uint16_t newest_seq_ = 0;

    std::map<uint16_t, MissingPacket> missing_packets_;

    uint32_t rtt_ms_ = 0;
    uint64_t last_failure_callback_ms_ = 0;
    Stats stats_;
    NackCallback nack_callback_;
    RecoveryFailureCallback recovery_failure_callback_;
};

}

#endif /* _NACK_REQUESTER_H_ */
