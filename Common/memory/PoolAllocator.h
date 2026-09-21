#ifndef PACKETIA_COMMON_MEMORY_POOLALLOCATOR_H_
#define PACKETIA_COMMON_MEMORY_POOLALLOCATOR_H_
#include "MemoryPool.h"
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace common
{
// STL adapter for private growable byte/character buffers. The allocator keeps
// the pool alive through container copies, moves and cross-thread destruction.
template<class T> class PoolAllocator
{
    static_assert(alignof(T) <= alignof(std::max_align_t), "over-aligned objects are not supported");
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    PoolAllocator() : pool_(MemoryPool::Default()) {}
    explicit PoolAllocator(const MemoryPool& pool) : pool_(pool) {}
    PoolAllocator(const PoolAllocator&) = default;
    PoolAllocator& operator=(const PoolAllocator&) = default;
    // Moved-from containers must still be able to allocate again.
    PoolAllocator(PoolAllocator&& other) noexcept : pool_(other.pool_) {}
    PoolAllocator& operator=(PoolAllocator&& other) noexcept { pool_ = other.pool_; return *this; }
    template<class U> PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool_) {}
    T* allocate(size_t count)
    {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        auto allocation = pool_.Allocate(count * sizeof(T));
        return reinterpret_cast<T*>(allocation.release());
    }
    void deallocate(T* ptr, size_t) noexcept { pool_.Deallocate(ptr); }
    template<class U> bool operator==(const PoolAllocator<U>& other) const noexcept
    { return pool_.SharesStateWith(other.pool_); }
    template<class U> bool operator!=(const PoolAllocator<U>& other) const noexcept
    { return !(*this == other); }
private:
    template<class> friend class PoolAllocator;
    MemoryPool pool_;
};
template<class T> using PoolVector = std::vector<T, PoolAllocator<T>>;
using ByteVector = PoolVector<uint8_t>;
} // namespace common

#endif // PACKETIA_COMMON_MEMORY_POOLALLOCATOR_H_
