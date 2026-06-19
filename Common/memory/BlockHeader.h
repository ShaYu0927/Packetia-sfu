#ifndef _BLOCK_HEADER_H_
#define _BLOCK_HEADER_H_

#include <cstdint>

namespace common 
{
struct BlockHeader 
{
    uint32_t magic;
    uint16_t class_index;
    uint16_t flags;
    uint32_t request_size;

    void* owner;
    BlockHeader* next;
};

static constexpr uint32_t kMagicUsed = 0xABCD1234;
static constexpr uint32_t kMagicFree = 0xDEAD5678;

static constexpr uint16_t kFlagLarge = 0x01;

}


#endif /* _BLOCK_HEADER_H_ */