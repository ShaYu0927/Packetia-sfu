#include "MemoryPool.h"
#include "CentralCache.h"
#include "ThreadCache.h"
#include <new>

namespace common
{
MemoryPool::MemoryPool(const MemoryPoolOptions& options)
    : state_(std::make_shared<PoolState>(options)) {}
MemoryPool& MemoryPool::Default()
{
    static MemoryPool pool;
    return pool;
}
void PoolDeleter::operator()(std::uint8_t* ptr) const noexcept
{
    if (ptr) state->central.Release(reinterpret_cast<BlockHeader*>(ptr) - 1);
}
PoolAllocation MemoryPool::Allocate(std::size_t size) const
{
    auto allocation = TryAllocate(size);
    if (size && !allocation) throw std::bad_alloc();
    return allocation;
}
PoolAllocation MemoryPool::TryAllocate(std::size_t size) const noexcept
{
    if (!size) return {};
    BlockHeader* block = nullptr;
    if (size > kMaxPooledObjectSize)
    {
        GetThreadCache().FlushFor(state_);
        block = state_->central.AllocateLarge(size);
    }
    else block = GetThreadCache().Allocate(state_, size);
    if (!block) state_->central.RecordFailure();
    return PoolAllocation(block ? reinterpret_cast<std::uint8_t*>(block + 1) : nullptr,
                          PoolDeleter{state_, block ? GetAllocationCapacity(size) : 0});
}
void MemoryPool::Deallocate(void* ptr) const noexcept
{
    if (ptr) state_->central.Release(reinterpret_cast<BlockHeader*>(ptr) - 1);
}
MemoryPoolStats MemoryPool::Stats() const noexcept { return state_->central.Stats(); }
MemoryPoolOptions MemoryPool::Options() const noexcept { return state_->central.Options(); }
bool MemoryPool::SetLimits(const MemoryPoolOptions& options) const noexcept
{
    GetThreadCache().FlushFor(state_);
    return state_->central.SetLimits(options);
}
std::size_t MemoryPool::Trim() const noexcept
{
    const auto before = Stats().committed_bytes;
    GetThreadCache().FlushFor(state_);
    state_->central.Trim();
    const auto after = Stats().committed_bytes;
    return before > after ? before - after : 0;
}
void MemoryPool::ReleaseThreadCache() noexcept { GetThreadCache().Flush(); }
} // namespace common
