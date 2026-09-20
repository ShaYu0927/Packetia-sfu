#pragma once
#include <cstddef>

namespace common
{
inline constexpr std::size_t kNumSizeClasses = 15;
inline constexpr std::size_t kMaxSmallObjectSize = 4096;
inline constexpr std::size_t kMaxPooledObjectSize = 1024 * 1024;

inline std::size_t GetSizeClassIndex(std::size_t size)
{
    std::size_t index = 0;
    std::size_t capacity = 64;
    while (capacity < size && index < kNumSizeClasses) { capacity *= 2; ++index; }
    return index;
}
inline std::size_t GetClassSize(std::size_t index) { return std::size_t{64} << index; }
inline std::size_t GetBatchCount(std::size_t index)
{
    const auto count = 4096 / GetClassSize(index);
    return count ? count : 1;
}
inline std::size_t GetAllocationCapacity(std::size_t size)
{
    return !size || size > kMaxPooledObjectSize ? size : GetClassSize(GetSizeClassIndex(size));
}
} // namespace common
