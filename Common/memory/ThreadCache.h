#ifndef _THREAD_CACHE_H_
#define _THREAD_CACHE_H_

#include <cstddef>
#include "BlockHeader.h"
#include "SizeClass.h"

namespace common 
{
class ThreadCache 
{
public:
    void* Alloc(std::size_t size);
    void Free(void* ptr);

private:
    struct LocalList 
    {
        BlockHeader* head = nullptr;
        std::size_t count = 0;
    };

private:
    BlockHeader* Pop(std::size_t class_index);
    void Push(std::size_t class_index, BlockHeader* block);

    void Refill(std::size_t class_index);
    void MaybeReleaseToCentral(std::size_t class_index);

private:
    LocalList lists_[kNumSizeClasses];
};

}


#endif /* _THREAD_CACHE_H_ */