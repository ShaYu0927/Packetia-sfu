#ifndef _CENTRAL_CACHE_H_
#define _CENTRAL_CACHE_H_

#include <mutex>
#include <cstddef>
#include "BlockHeader.h"
#include "SizeClass.h"

namespace common 
{

class CentralCache 
{
public:
    static CentralCache& Instance();

    BlockHeader* FetchBatch(std::size_t class_index, std::size_t batch_count);
    void ReleaseBatch(std::size_t class_index, BlockHeader* head, std::size_t count);

private:
    struct FreeList 
    {
        std::mutex mutex;
        BlockHeader* head = nullptr;
        std::size_t count = 0;
    };

private:
    CentralCache() = default;

    BlockHeader* AllocateFromSystem(std::size_t class_index, std::size_t batch_count);

private:
    FreeList lists_[kNumSizeClasses];
};

}


#endif /* _CENTRAL_CACHE_H_ */