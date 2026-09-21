#ifndef PACKETIA_COMMON_MEMORY_THREADCACHE_H_
#define PACKETIA_COMMON_MEMORY_THREADCACHE_H_
#include "BlockHeader.h"
#include "MemoryPool.h"
#include "SizeClass.h"

namespace common
{
// One cache per thread, bound to at most one pool. Switching pools flushes it,
// so short-lived pool handles cannot create an unbounded TLS registry.
class ThreadCache
{
public:
    ~ThreadCache();
    ThreadCache() = default;
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;
    BlockHeader* Allocate(const std::shared_ptr<PoolState>& state, std::size_t size) noexcept;
    void Flush() noexcept;
    void FlushFor(const std::shared_ptr<PoolState>& state) noexcept;
private:
    std::shared_ptr<PoolState> state_;
    BlockHeader* lists_[kNumSizeClasses]{};
};
ThreadCache& GetThreadCache();
} // namespace common

#endif // PACKETIA_COMMON_MEMORY_THREADCACHE_H_
