#include "CentralCache.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>

namespace common
{
struct alignas(std::max_align_t) PoolSlab
{
    PoolSlab* next = nullptr;
    BlockHeader* free = nullptr;
    std::size_t capacity = 0;
    std::size_t stride = 0;
    std::size_t count = 0;
    std::size_t free_count = 0;
    std::size_t bytes = 0;
    std::size_t class_index = 0;
};

CentralCache::~CentralCache()
{
    // Every live lease and local cache retains PoolState.
    assert(stats_.live_allocations == 0);
    for (auto& head : slabs_) while (head) FreeSlab(&head);
}

PoolSlab* CentralCache::AllocateSlab(std::size_t capacity, std::size_t count,
                                    std::size_t class_index) noexcept
{
    constexpr auto alignment = alignof(std::max_align_t);
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    if (capacity > max - sizeof(BlockHeader) - (alignment - 1)) return nullptr;
    const auto stride = (sizeof(BlockHeader) + capacity + alignment - 1) / alignment * alignment;
    if (stride > max - sizeof(PoolSlab)) return nullptr;
    if (options_.max_committed_bytes - stats_.committed_bytes < sizeof(PoolSlab) + stride)
        TrimLocked(true);
    const auto available = options_.max_committed_bytes - stats_.committed_bytes;
    if (available < sizeof(PoolSlab) + stride) return nullptr;
    count = std::min(count, (available - sizeof(PoolSlab)) / stride);
    const auto bytes = sizeof(PoolSlab) + count * stride;
    auto* allocation = std::malloc(bytes);
    if (!allocation) return nullptr;
    auto* slab = new (allocation) PoolSlab;
    slab->capacity = capacity;
    slab->stride = stride;
    slab->count = slab->free_count = count;
    slab->bytes = bytes;
    slab->class_index = class_index;
    auto* raw = reinterpret_cast<unsigned char*>(slab + 1);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto* block = new (raw + i * stride) BlockHeader;
        block->slab = slab;
        block->magic = kMagicFree;
        block->next = slab->free;
        slab->free = block;
    }
    slab->next = slabs_[class_index];
    slabs_[class_index] = slab;
    stats_.committed_bytes += bytes;
    stats_.peak_committed_bytes = std::max(stats_.peak_committed_bytes, stats_.committed_bytes);
    ++stats_.slab_count;
    ++stats_.system_allocations;
    return slab;
}

BlockHeader* CentralCache::FetchBatch(std::size_t class_index, std::size_t count) noexcept
{
    assert(class_index < kNumSizeClasses && count);
    std::lock_guard<std::mutex> lock(mutex_);
    auto* slab = slabs_[class_index];
    while (slab && !slab->free) slab = slab->next;
    if (!slab) slab = AllocateSlab(GetClassSize(class_index), count, class_index);
    if (!slab) return nullptr;
    BlockHeader* result = nullptr;
    while (count-- && slab->free)
    {
        auto* block = slab->free;
        slab->free = block->next;
        --slab->free_count;
        block->next = result;
        result = block;
    }
    return result;
}

void CentralCache::ActivateLocked(BlockHeader* block, std::size_t size) noexcept
{
    assert(block->magic == kMagicFree);
    block->magic = kMagicUsed;
    block->request_size = size;
    stats_.in_use_bytes += size;
    stats_.in_use_capacity += block->slab->capacity;
    in_use_reserved_ += block->slab->stride;
    ++stats_.live_allocations;
    stats_.peak_in_use_bytes = std::max(stats_.peak_in_use_bytes, stats_.in_use_bytes);
}

void CentralCache::Activate(BlockHeader* block, std::size_t size) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ActivateLocked(block, size);
}

BlockHeader* CentralCache::AllocateLarge(std::size_t size) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto* slab = AllocateSlab(size, 1, kNumSizeClasses);
    if (!slab) return nullptr;
    auto* block = slab->free;
    slab->free = nullptr;
    slab->free_count = 0;
    ActivateLocked(block, size);
    return block;
}

void CentralCache::ReleaseBatch(BlockHeader* head) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (head)
    {
        auto* block = head;
        head = head->next;
        assert(block->magic == kMagicFree);
        block->next = block->slab->free;
        block->slab->free = block;
        ++block->slab->free_count;
    }
    TrimLocked(false);
}

void CentralCache::Release(BlockHeader* block) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    assert(block->magic == kMagicUsed);
    auto* slab = block->slab;
    stats_.in_use_bytes -= block->request_size;
    stats_.in_use_capacity -= slab->capacity;
    in_use_reserved_ -= slab->stride;
    --stats_.live_allocations;
    block->magic = kMagicFree;
    block->request_size = 0;
    block->next = slab->free;
    slab->free = block;
    ++slab->free_count;
    if (slab->class_index == kNumSizeClasses)
    {
        auto** link = &slabs_[kNumSizeClasses];
        while (*link != slab) link = &(*link)->next;
        FreeSlab(link);
    }
    else TrimLocked(false);
}

void CentralCache::FreeSlab(PoolSlab** link) noexcept
{
    auto* slab = *link;
    *link = slab->next;
    stats_.committed_bytes -= slab->bytes;
    --stats_.slab_count;
    std::free(slab);
}

std::size_t CentralCache::TrimLocked(bool all) noexcept
{
    const auto before = stats_.committed_bytes;
    // Large classes first so a single large frame doesn't evict every packet.
    for (std::size_t i = kNumSizeClasses; i-- > 0;)
    {
        auto** link = &slabs_[i];
        while (*link)
        {
            if (!all && stats_.committed_bytes - in_use_reserved_ <= options_.max_cached_bytes)
                return before - stats_.committed_bytes;
            if ((*link)->free_count == (*link)->count) FreeSlab(link);
            else link = &(*link)->next;
        }
    }
    return before - stats_.committed_bytes;
}

std::size_t CentralCache::Trim() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TrimLocked(true);
}

MemoryPoolStats CentralCache::Stats() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_;
    result.cached_bytes = stats_.committed_bytes - in_use_reserved_;
    return result;
}
MemoryPoolOptions CentralCache::Options() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}
bool CentralCache::SetLimits(MemoryPoolOptions options) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrimLocked(true);
    if (options.max_committed_bytes < stats_.committed_bytes) return false;
    options_ = options;
    return true;
}
void CentralCache::RecordFailure() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.allocation_failures;
}
} // namespace common
