#include "CentralCache.h"
#include <cstdlib>
#include <limits>

namespace common
{

static std::size_t AlignUp(std::size_t size, std::size_t align)
{
    return ((size + align - 1) / align) * align;
}

static void AppendNode(BlockHeader*& head, BlockHeader*& tail, BlockHeader* node)
{
    node->next = nullptr;

    if (!head) 
    {
        head = node;
        tail = node;
        return;
    }

    tail->next = node;
    tail = node;
}

CentralCache& CentralCache::Instance()
{
    static CentralCache cache;
    return cache;
}

BlockHeader* CentralCache::AllocateFromSystem(std::size_t class_index, std::size_t batch_count)
{
    if (class_index >= kNumSizeClasses || batch_count == 0) {
        return nullptr;
    }

    const std::size_t class_size = GetClassSize(class_index);

    const std::size_t block_size = AlignUp(sizeof(BlockHeader) + class_size, alignof(std::max_align_t));

    if (batch_count > std::numeric_limits<std::size_t>::max() / block_size) 
    {
        return nullptr;
    }

    const std::size_t total_size = block_size * batch_count;

    char* raw = static_cast<char*>(std::malloc(total_size));
    if (!raw) 
    {
        return nullptr;
    }

    BlockHeader* head = nullptr;
    for (std::size_t i = 0; i < batch_count; ++i) 
    {
        char* block_addr = raw + i * block_size;

        BlockHeader* header = reinterpret_cast<BlockHeader*>(block_addr);

        header->magic = kMagicFree;
        header->class_index = static_cast<uint16_t>(class_index);
        header->flags = 0;
        header->request_size = 0;
        header->owner = nullptr;

        header->next = head;
        head = header;
    }
    return head;

}


void CentralCache::ReleaseBatch(std::size_t class_index, BlockHeader* head, std::size_t count)
{
    if (class_index >= kNumSizeClasses || !head || count == 0) 
    {
        return;
    }

    BlockHeader* tail = nullptr;
    BlockHeader* cur = head;
    std::size_t real_count = 0;

    while (cur) 
    {
        cur->magic = kMagicFree;
        cur->class_index = static_cast<uint16_t>(class_index);
        cur->flags = 0;
        cur->request_size = 0;
        cur->owner = nullptr;

        tail = cur;
        cur = cur->next;
        ++real_count;
    }

    FreeList& list = lists_[class_index];

    {
        std::lock_guard<std::mutex> lock(list.mutex);

        tail->next = list.head;
        list.head = head;
        list.count += real_count;
    }
}


BlockHeader* CentralCache::FetchBatch(std::size_t class_index, std::size_t batch_count)
{
    if (class_index >= kNumSizeClasses || batch_count == 0) 
    {
        return nullptr;
    }

    BlockHeader* result_head = nullptr;
    BlockHeader* result_tail = nullptr;
    std::size_t got_count = 0;

    {
        FreeList& list = lists_[class_index];
        std::lock_guard<std::mutex> lock(list.mutex);

        while (got_count < batch_count && list.head) 
        {
            BlockHeader* node = list.head;
            list.head = node->next;
            --list.count;

            AppendNode(result_head, result_tail, node);
            ++got_count;
        }
    }

    if (got_count == batch_count) 
    {
        return result_head;
    }

    const std::size_t need_count = batch_count - got_count;
    BlockHeader* more = AllocateFromSystem(class_index, need_count);

    if (!more) 
    {
        if (result_head) 
        {
            ReleaseBatch(class_index, result_head, got_count);
        }

        return nullptr;
    }

    if (!result_head) 
    {
        return more;
    }

    result_tail->next = more;
    return result_head;
}

}
