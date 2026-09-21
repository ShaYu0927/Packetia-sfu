#ifndef PACKETIA_COMMON_MEMORY_MUTABLEBUFFER_H_
#define PACKETIA_COMMON_MEMORY_MUTABLEBUFFER_H_
#include "SharedBuffer.h"

namespace common
{
// Exclusive writable storage. Allocate once, fill, then Freeze to publish.
// Previously obtained mutable pointers must not be used after Freeze.
class MutableBuffer
{
public:
    MutableBuffer() = default;
    MutableBuffer(const MutableBuffer&) = delete;
    MutableBuffer& operator=(const MutableBuffer&) = delete;
    MutableBuffer(MutableBuffer&& other) noexcept
        : storage_(std::move(other.storage_)), size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}
    MutableBuffer& operator=(MutableBuffer&& other) noexcept
    {
        if (this != &other)
        {
            storage_ = std::move(other.storage_);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }
    static MutableBuffer Allocate(size_t size, const MemoryPool& pool = MemoryPool::Default())
    {
        MutableBuffer result;
        result.storage_ = pool.Allocate(size);
        result.capacity_ = result.size_ = size;
        return result;
    }
    static MutableBuffer Copy(BufferView view, const MemoryPool& pool = MemoryPool::Default())
    {
        auto result = Allocate(view.Size(), pool);
        if (!view.Empty()) std::memcpy(result.Data(), view.Data(), view.Size());
        return result;
    }
    uint8_t* Data() noexcept { return storage_.get(); }
    const uint8_t* Data() const noexcept { return storage_.get(); }
    size_t Size() const noexcept { return size_; }
    size_t Capacity() const noexcept { return capacity_; }
    BufferView View() const noexcept { return BufferView(Data(), size_); }
    void Resize(size_t size)
    {
        if (size > capacity_) throw std::out_of_range("buffer resize exceeds capacity");
        size_ = size;
    }
    void Append(BufferView view)
    {
        if (view.Size() > capacity_ - size_) throw std::out_of_range("buffer append exceeds capacity");
        if (!view.Empty()) std::memmove(Data() + size_, view.Data(), view.Size());
        size_ += view.Size();
    }
    SharedBuffer Freeze() &&
    {
        const auto size = std::exchange(size_, 0);
        capacity_ = 0;
        return SharedBuffer::Take(std::move(storage_), size);
    }
private:
    PoolAllocation storage_;
    size_t size_ = 0;
    size_t capacity_ = 0;
};
} // namespace common

#endif // PACKETIA_COMMON_MEMORY_MUTABLEBUFFER_H_
