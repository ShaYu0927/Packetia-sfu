#ifndef _SIZE_CLASS_H_
#define _SIZE_CLASS_H_

#include <cstddef>

namespace common 
{
static constexpr std::size_t kNumSizeClasses = 7;
static constexpr std::size_t kMaxSmallObjectSize = 4096;

inline std::size_t GetSizeClassIndex(std::size_t size)
{
    if (size <= 64) return 0;
    if (size <= 128) return 1;
    if (size <= 256) return 2;
    if (size <= 512) return 3;
    if (size <= 1024) return 4;
    if (size <= 2048) return 5;
    return 6;
}

inline std::size_t GetClassSize(std::size_t index)
{
    static constexpr std::size_t sizes[kNumSizeClasses] = {
        64, 128, 256, 512, 1024, 2048, 4096
    };

    return sizes[index];
}

inline std::size_t GetBatchCount(std::size_t index)
{
    static constexpr std::size_t batch[kNumSizeClasses] = {
        64, 64, 32, 32, 16, 8, 4
    };

    return batch[index];
}
}


#endif /* _SIZE_CLASS_H_ */