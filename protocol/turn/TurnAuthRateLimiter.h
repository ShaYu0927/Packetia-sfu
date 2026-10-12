#pragma once

#include "UdpSocket.h"
#include "TurnConstants.h"

#include <array>

namespace protocol
{

// Owned by one TURN scheduler, just like its sessions. No concurrent calls.
// Direct-mapped buckets share an active budget on collision; source ports
// never participate in the key. The request path performs no allocation.
class TurnAuthRateLimiter
{
public:
    static constexpr size_t kBucketCount = PACKETIA_TURN_RATELIMIT_BUCKETS;
    static_assert(kBucketCount != 0 && (kBucketCount & (kBucketCount - 1)) == 0,
                  "Rate-limit bucket count must be a power of two");
    struct Decision
    {
        bool suppressed = false;
        bool collision = false;
        bool first_drop = false;
        bool first_collision = false;
    };

    Decision Consume(const network::SocketAddr& source, uint64_t now_ms, uint32_t max_per_second);
    size_t OccupiedBuckets(uint64_t now_ms) const;
    void Reset();

private:
    struct Bucket
    {
        uint64_t window_start_ms = 0;
        uint32_t tag = 0;
        uint32_t count = 0;
        bool drop_logged = false;
        bool collision_logged = false;
    };
    std::array<Bucket, kBucketCount> buckets_{};
};

} // namespace protocol
