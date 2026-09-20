#pragma once
#include "BlockHeader.h"
#include "MemoryPool.h"
#include "SizeClass.h"
#include <mutex>

namespace common
{
// Owned by PoolState, never a process-lifetime raw singleton.
class CentralCache
{
public:
    explicit CentralCache(MemoryPoolOptions options) : options_(options) {}
    ~CentralCache();
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    BlockHeader* FetchBatch(std::size_t class_index, std::size_t count) noexcept;
    void ReleaseBatch(BlockHeader* head) noexcept;
    void Activate(BlockHeader* block, std::size_t size) noexcept;
    BlockHeader* AllocateLarge(std::size_t size) noexcept;
    void Release(BlockHeader* block) noexcept;
    MemoryPoolStats Stats() const noexcept;
    MemoryPoolOptions Options() const noexcept;
    bool SetLimits(MemoryPoolOptions options) noexcept;
    void RecordFailure() noexcept;
    std::size_t Trim() noexcept;

private:
    PoolSlab* AllocateSlab(std::size_t capacity, std::size_t count,
                           std::size_t class_index) noexcept;
    std::size_t TrimLocked(bool all) noexcept;
    void FreeSlab(PoolSlab** link) noexcept;
    void ActivateLocked(BlockHeader* block, std::size_t size) noexcept;

    MemoryPoolOptions options_;
    mutable std::mutex mutex_;
    PoolSlab* slabs_[kNumSizeClasses + 1]{};
    MemoryPoolStats stats_;
    std::size_t in_use_reserved_ = 0;
};

class PoolState
{
public:
    explicit PoolState(MemoryPoolOptions options) : central(options) {}
    CentralCache central;
};
} // namespace common
