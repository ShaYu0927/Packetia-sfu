#pragma once
#include <cstddef>
#include <cstdint>

namespace common
{
struct PoolSlab;
// Payload begins immediately after the header and is max_align_t aligned.
struct alignas(std::max_align_t) BlockHeader
{
    PoolSlab* slab = nullptr;
    BlockHeader* next = nullptr;
    std::size_t request_size = 0;
    std::uint32_t magic = 0;
};
inline constexpr std::uint32_t kMagicUsed = 0xABCD1234;
inline constexpr std::uint32_t kMagicFree = 0xDEAD5678;
static_assert(sizeof(BlockHeader) % alignof(std::max_align_t) == 0);
} // namespace common
