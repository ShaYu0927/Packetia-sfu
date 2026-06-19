#include "ThreadCache.h"
#include "BlockHeader.h"
#include "CentralCache.h"

namespace common
{
inline BlockHeader* PtrToHeader(void* ptr)
{
    return reinterpret_cast<BlockHeader*>(ptr) - 1;
}

inline void* HeaderToPtr(BlockHeader* header)
{
    return reinterpret_cast<void*>(header + 1);
}

ThreadCache* GetThreadCache()
{
    thread_local ThreadCache cache;
    return &cache;
}

void* ThreadCache::Alloc(std::size_t size)
{
    if (size == 0 || size > kMaxSmallObjectSize)
    {
        return nullptr;
    }

    const std::size_t class_index = GetSizeClassIndex(size);

    BlockHeader* block = Pop(class_index);
    if (!block) 
    {
        Refill(class_index);
        block = Pop(class_index);
    }

    if (!block) 
    {
        return nullptr;
    }

    block->magic = kMagicUsed;
    block->class_index = static_cast<uint16_t>(class_index);
    block->flags = 0;
    block->request_size = static_cast<uint32_t>(size);
    block->owner = this;
    block->next = nullptr;

    return HeaderToPtr(block);
}

void ThreadCache::Free(void* ptr)
{
    if (!ptr) 
    {
        return;
    }

    BlockHeader* block = PtrToHeader(ptr);

    if (block->magic != kMagicUsed) 
    {
        return;
    }

    const std::size_t class_index = block->class_index;
    if (class_index >= kNumSizeClasses) 
    {
        return;
    }

    block->magic = kMagicFree;
    block->request_size = 0;

    if (block->owner != this) 
    {
        block->owner = nullptr;
        block->next = nullptr;

        CentralCache::Instance().ReleaseBatch(class_index, block, 1);
        return;
    }

    Push(class_index, block);
    MaybeReleaseToCentral(class_index);
}

BlockHeader* ThreadCache::Pop(std::size_t class_index)
{
    if (class_index >= kNumSizeClasses) 
    {
        return nullptr;
    }

    LocalList& list = lists_[class_index];

    BlockHeader* block = list.head;
    if (!block) 
    {
        return nullptr;
    }

    list.head = block->next;
    block->next = nullptr;

    if (list.count > 0) 
    {
        --list.count;
    }

    return block;
}

void ThreadCache::Push(std::size_t class_index, BlockHeader* block)
{
    if (class_index >= kNumSizeClasses || !block) 
    {
        return;
    }

    LocalList& list = lists_[class_index];

    block->next = list.head;
    list.head = block;
    ++list.count;
}

void ThreadCache::Refill(std::size_t class_index)
{
    if (class_index >= kNumSizeClasses) 
    {
        return;
    }

    const std::size_t batch_count = GetBatchCount(class_index);

    BlockHeader* batch = CentralCache::Instance().FetchBatch(class_index, batch_count);

    if (!batch) 
    {
        return;
    }

    LocalList& list = lists_[class_index];

    BlockHeader* cur = batch;
    while (cur) 
    {
        BlockHeader* next = cur->next;

        cur->magic = kMagicFree;
        cur->class_index = static_cast<uint16_t>(class_index);
        cur->flags = 0;
        cur->request_size = 0;
        cur->owner = this;

        cur->next = list.head;
        list.head = cur;
        ++list.count;

        cur = next;
    }
}

void ThreadCache::MaybeReleaseToCentral(std::size_t class_index)
{
    if (class_index >= kNumSizeClasses) 
    {
        return;
    }

    LocalList& list = lists_[class_index];

    const std::size_t batch_count = GetBatchCount(class_index);
    const std::size_t max_local_count = batch_count * 2;

    if (list.count <= max_local_count) 
    {
        return;
    }

    BlockHeader* release_head = nullptr;
    BlockHeader* release_tail = nullptr;
    std::size_t release_count = 0;

    while (release_count < batch_count && list.head) 
    {
        BlockHeader* block = list.head;
        list.head = block->next;

        if (list.count > 0) 
        {
            --list.count;
        }

        block->next = nullptr;
        block->owner = nullptr;
        block->magic = kMagicFree;
        block->request_size = 0;

        if (!release_head) 
        {
            release_head = block;
            release_tail = block;
        } 
        else 
        {
            release_tail->next = block;
            release_tail = block;
        }

        ++release_count;
    }

    if (release_head) 
    {
        CentralCache::Instance().ReleaseBatch(class_index, release_head, release_count);
    }
}
}