#ifndef PACKETIA_COMMON_MEMORY_MEMORYPOOL_H_
#define PACKETIA_COMMON_MEMORY_MEMORYPOOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace common
{
class PoolState;

struct MemoryPoolOptions
{
    // Includes slab metadata, block headers and rounded byte capacity.
    std::size_t max_committed_bytes = 256 * 1024 * 1024;
    // Retention target, not an additional budget. Active/locally cached blocks
    // can pin slabs above this target; max_committed_bytes is always enforced.
    std::size_t max_cached_bytes = 16 * 1024 * 1024;
};

struct MemoryPoolStats
{
    std::size_t committed_bytes = 0;
    std::size_t peak_committed_bytes = 0;
    std::size_t in_use_bytes = 0;
    std::size_t peak_in_use_bytes = 0;
    std::size_t in_use_capacity = 0;
    std::size_t cached_bytes = 0;
    std::size_t live_allocations = 0;
    std::size_t slab_count = 0;
    std::uint64_t allocation_failures = 0;
    std::uint64_t system_allocations = 0;
};

struct PoolDeleter
{
    std::shared_ptr<PoolState> state;
    std::size_t capacity = 0;
    void operator()(std::uint8_t* ptr) const noexcept;
};
using PoolAllocation = std::unique_ptr<std::uint8_t[], PoolDeleter>;

// Cheap shared handle. A lease keeps state alive independently of this handle
// and of the allocating thread. No deleter ever dereferences a ThreadCache.
class MemoryPool
{
public:
    explicit MemoryPool(const MemoryPoolOptions& options = {});
    static MemoryPool& Default();

    // Zero => empty lease. Exhaustion/overflow => bad_alloc; never bypasses
    // the budget via an unaccounted heap fallback.
    PoolAllocation Allocate(std::size_t size) const;
    PoolAllocation TryAllocate(std::size_t size) const noexcept;
    MemoryPoolStats Stats() const noexcept;
    MemoryPoolOptions Options() const noexcept;
    // Returns false if live/local allocations prevent lowering the limit.
    bool SetLimits(const MemoryPoolOptions& options) const noexcept;
    // Flush this thread's cache and release fully idle slabs. Other threads'
    // local blocks are returned when they exit, switch pools, or flush.
    std::size_t Trim() const noexcept;
    static void ReleaseThreadCache() noexcept;
    bool SharesStateWith(const MemoryPool& other) const noexcept { return state_ == other.state_; }

private:
    template<class> friend class PoolAllocator;
    void Deallocate(void* ptr) const noexcept;
    std::shared_ptr<PoolState> state_;
};
} // namespace common

#endif // PACKETIA_COMMON_MEMORY_MEMORYPOOL_H_
