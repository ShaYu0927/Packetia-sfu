#ifndef PACKETIA_COMMON_MEMORY_SHAREDBUFFER_H_
#define PACKETIA_COMMON_MEMORY_SHAREDBUFFER_H_

#include "MemoryPool.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace common
{

// A synchronous, read-only borrow. The caller must keep the backing storage
// alive and unchanged; storing this view does not extend that lifetime.
class BufferView
{
public:
    BufferView() = default;
    BufferView(const uint8_t* data, size_t size) : data_(size ? data : nullptr), size_(size)
    {
        if (size && !data) throw std::invalid_argument("null buffer with nonzero size");
    }

    const uint8_t* Data() const noexcept { return data_; }
    size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }

    BufferView Slice(size_t offset, size_t size) const
    {
        if (offset > size_ || size > size_ - offset)
            throw std::out_of_range("buffer slice exceeds its bounds");
        return size ? BufferView(data_ + offset, size) : BufferView{};
    }

    std::vector<uint8_t> ToVector() const
    {
        return Empty() ? std::vector<uint8_t>{} : std::vector<uint8_t>(data_, data_ + size_);
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// An immutable, owning byte range. Copies and slices share storage; the last
// owner releases it on whichever thread drops the final reference. Mutating
// protocols build a private MutableBuffer and publish it with Freeze().
class SharedBuffer
{
public:
    SharedBuffer() = default;
    SharedBuffer(const SharedBuffer&) = default;
    SharedBuffer& operator=(const SharedBuffer&) = default;
    SharedBuffer(SharedBuffer&& other) noexcept
        : owner_(std::move(other.owner_)), data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)), storage_size_(std::exchange(other.storage_size_, 0)) {}
    SharedBuffer& operator=(SharedBuffer&& other) noexcept
    {
        if (this != &other)
        {
            owner_ = std::move(other.owner_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            storage_size_ = std::exchange(other.storage_size_, 0);
        }
        return *this;
    }

    static SharedBuffer Copy(BufferView view, const MemoryPool& pool = MemoryPool::Default())
    {
        if (view.Empty()) return {};
        auto bytes = pool.Allocate(view.Size());
        std::memcpy(bytes.get(), view.Data(), view.Size());
        return Take(std::move(bytes), view.Size());
    }
    static SharedBuffer Copy(const uint8_t* data, size_t size,
                             const MemoryPool& pool = MemoryPool::Default())
    {
        return Copy(BufferView(data, size), pool);
    }

    // Network admission: exhaustion drops the packet instead of escaping the
    // IO callback. Invalid inputs still report a programming error.
    static SharedBuffer TryCopy(const uint8_t* data, size_t size,
                                const MemoryPool& pool = MemoryPool::Default())
    {
        try { return Copy(data, size, pool); }
        catch (const std::bad_alloc&) { return {}; }
    }

    // Explicit foreign-storage adapter; this allocation is NOT pool budgeted.
    // Passing an lvalue copies it; std::move transfers its byte allocation.
    // Previously obtained mutable pointers must not be used after transfer.
    static SharedBuffer Take(std::vector<uint8_t> bytes)
    {
        if (bytes.empty()) return {};
        auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        const auto* data = storage->data();
        const auto size = storage->size();
        const auto capacity = storage->capacity();
        return SharedBuffer(std::move(storage), data, size, capacity);
    }

    static SharedBuffer Take(PoolAllocation bytes, size_t size)
    {
        if (size && !bytes) throw std::invalid_argument("null owned buffer");
        const auto capacity = bytes.get_deleter().capacity;
        if (size > capacity) throw std::out_of_range("pool lease size exceeds capacity");
        if (!size) return {};
        const auto* data = bytes.get();
        std::shared_ptr<const uint8_t[]> storage(std::move(bytes));
        std::shared_ptr<const void> owner(storage, data);
        return SharedBuffer(std::move(owner), data, size, capacity);
    }

    // Adapter for an allocator/pool lease. The deleter must be noexcept, safe
    // on any releasing thread, and retain the pool state it needs. size must
    // fit the allocation; no borrowed pointer can be adopted through this API.
    template<class Deleter>
    static SharedBuffer Take(std::unique_ptr<uint8_t[], Deleter> bytes, size_t size)
    {
        if (size && !bytes) throw std::invalid_argument("null owned buffer");
        if (!size) return {};
        const auto* data = bytes.get();
        std::shared_ptr<const uint8_t[]> storage(std::move(bytes));
        std::shared_ptr<const void> owner(storage, data);
        return SharedBuffer(std::move(owner), data, size);
    }

    const uint8_t* Data() const noexcept { return data_; }
    size_t Size() const noexcept { return size_; }
    bool Empty() const noexcept { return size_ == 0; }
    explicit operator bool() const noexcept { return !Empty(); }
    BufferView View() const noexcept { return BufferView(data_, size_); }

    SharedBuffer Slice(size_t offset, size_t size) const
    {
        const auto view = View().Slice(offset, size);
        return size ? SharedBuffer(owner_, view.Data(), size, storage_size_) : SharedBuffer{};
    }

    // Conservative retained byte charge: a tiny slice pins its whole owner.
    size_t StorageSize() const noexcept { return storage_size_; }

    // Explicit copy for a consumer that needs writable storage (e.g. crypto).
    std::vector<uint8_t> ToVector() const { return View().ToVector(); }

    friend bool operator==(const SharedBuffer& a, const SharedBuffer& b) noexcept
    {
        return a.size_ == b.size_ &&
            (a.Empty() || std::memcmp(a.data_, b.data_, a.size_) == 0);
    }
    friend bool operator!=(const SharedBuffer& a, const SharedBuffer& b) noexcept
    {
        return !(a == b);
    }

private:
    SharedBuffer(std::shared_ptr<const void> owner, const uint8_t* data, size_t size, size_t storage_size = 0)
        : owner_(std::move(owner)), data_(data), size_(size),
          storage_size_(storage_size ? storage_size : size) {}

    std::shared_ptr<const void> owner_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t storage_size_ = 0;
};

} // namespace common

#endif // PACKETIA_COMMON_MEMORY_SHAREDBUFFER_H_
