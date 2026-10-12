#include "TurnAuthRateLimiter.h"
#include "StunConstants.h"

// FNV-1a accumulation followed by the MurmurHash3 32-bit finalizer.
#define PACKETIA_TURN_HASH_FNV_OFFSET_BASIS 2166136261u
#define PACKETIA_TURN_HASH_FNV_PRIME 16777619u
#define PACKETIA_TURN_HASH_FMIX_MULTIPLIER_1 0x85ebca6bu
#define PACKETIA_TURN_HASH_FMIX_MULTIPLIER_2 0xc2b2ae35u
#define PACKETIA_TURN_HASH_FMIX_SHIFT_1 16u
#define PACKETIA_TURN_HASH_FMIX_SHIFT_2 13u

namespace protocol
{
namespace
{
uint32_t AddressTag(const network::SocketAddr& source)
{
    const auto address = source.Normalized();
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (address.IsV4())
    {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address.ss);
        bytes = reinterpret_cast<const uint8_t*>(&v4->sin_addr);
        size = PACKETIA_STUN_IPV4_ADDRESS_BYTES;
    }
    else
    {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address.ss);
        bytes = v6->sin6_addr.s6_addr;
        size = PACKETIA_STUN_IPV6_ADDRESS_BYTES;
    }
    uint32_t hash = PACKETIA_TURN_HASH_FNV_OFFSET_BASIS;
    hash = (hash ^ static_cast<uint32_t>(size)) * PACKETIA_TURN_HASH_FNV_PRIME;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * PACKETIA_TURN_HASH_FNV_PRIME;
    hash ^= hash >> PACKETIA_TURN_HASH_FMIX_SHIFT_1;
    hash *= PACKETIA_TURN_HASH_FMIX_MULTIPLIER_1;
    hash ^= hash >> PACKETIA_TURN_HASH_FMIX_SHIFT_2;
    hash *= PACKETIA_TURN_HASH_FMIX_MULTIPLIER_2;
    hash ^= hash >> PACKETIA_TURN_HASH_FMIX_SHIFT_1;
    return hash ? hash : 1;
}

bool Live(uint64_t start, uint64_t now)
{
    return now >= start && now - start < PACKETIA_TURN_RATELIMIT_WINDOW_MS;
}
} // namespace

TurnAuthRateLimiter::Decision TurnAuthRateLimiter::Consume(const network::SocketAddr& source, uint64_t now_ms, uint32_t max_per_second)
{
    Decision result;
    if (!source.IsValid() || !max_per_second)
    {
        result.suppressed = true;
        return result;
    }
    const auto tag = AddressTag(source);
    auto& bucket = buckets_[tag & (kBucketCount - 1)];
    if (!bucket.tag || !Live(bucket.window_start_ms, now_ms))
        bucket = {now_ms, tag, 0, false, false};
    if (bucket.tag != tag)
    {
        result.collision = true;
        result.first_collision = !bucket.collision_logged;
        bucket.collision_logged = true;
    }
    if (bucket.count < max_per_second)
        ++bucket.count;
    else
    {
        // Saturate: an arbitrarily long flood must not wrap the counter.
        result.suppressed = true;
        result.first_drop = !bucket.drop_logged;
        bucket.drop_logged = true;
    }
    return result;
}

size_t TurnAuthRateLimiter::OccupiedBuckets(uint64_t now_ms) const
{
    size_t count = 0;
    for (const auto& bucket : buckets_)
        if (bucket.tag && Live(bucket.window_start_ms, now_ms)) ++count;
    return count;
}

void TurnAuthRateLimiter::Reset()
{
    buckets_ = {};
}

} // namespace protocol
