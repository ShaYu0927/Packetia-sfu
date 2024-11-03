#include <iostream>
#include "utils/MemoryManager.h" // 确保包含头文件
#include <string.h>

int main() {
    const uint32_t blockSize = 1024; // 每个块的大小
    const int numBlocks = 10; // 块的数量

    // 初始化内存管理器
    MemoryManager& memManager = MemoryManager::Instance();

    // 测试内存分配
    std::cout << "Allocating memory blocks..." << std::endl;
    for (int i = 0; i < numBlocks; ++i) {
        void* ptr = Alloc(blockSize);
        if (ptr) {
            std::cout << "Allocated block " << i + 1 << " at address: " << ptr << std::endl;
            // 模拟使用分配的内存
            memset(ptr, 0, blockSize); // 将内存清零
        } else {
            std::cout << "Failed to allocate block " << i + 1 << std::endl;
        }
    }

    // 测试内存释放
    std::cout << "Freeing memory blocks..." << std::endl;
    for (int i = 0; i < numBlocks; ++i) {
        void* ptr = Alloc(blockSize); // 再次分配以获取地址用于释放
        Free(ptr);
        std::cout << "Freed block " << i + 1 << " at address: " << ptr << std::endl;
    }

    std::cout << "Memory allocation and deallocation test completed." << std::endl;

    return 0;
}



