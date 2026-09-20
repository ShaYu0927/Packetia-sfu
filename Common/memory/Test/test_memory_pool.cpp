#include "Common/memory/MemoryPool.h"
#include "Common/memory/MutableBuffer.h"
#include "Common/memory/PoolAllocator.h"
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
using namespace common;
namespace
{
void SizeClassesAndLargeFrames()
{
    MemoryPool pool({8 * 1024 * 1024, 2 * 1024 * 1024});
    const size_t sizes[] = {1, 63, 64, 65, 127, 128, 129, 1500, 4096, 4097,
                           65536, 1024 * 1024, 1024 * 1024 + 1};
    for (const auto size : sizes)
    {
        auto bytes = pool.Allocate(size);
        CHECK(reinterpret_cast<uintptr_t>(bytes.get()) % alignof(std::max_align_t) == 0);
        bytes[0] = 0x5a; bytes[size - 1] = 0xa5;
        CHECK(pool.Stats().in_use_bytes == size);
        CHECK(pool.Stats().live_allocations == 1);
        bytes.reset();
        CHECK(pool.Stats().in_use_bytes == 0);
        CHECK(pool.Stats().live_allocations == 0);
    }
    pool.Trim();
    CHECK(pool.Stats().committed_bytes == 0);
    CHECK(!pool.Allocate(0));
    CHECK(!pool.TryAllocate(std::numeric_limits<size_t>::max()));
    CHECK(pool.Stats().allocation_failures == 1);
    bool threw = false;
    try { pool.Allocate(9 * 1024 * 1024); } catch (const std::bad_alloc&) { threw = true; }
    CHECK(threw && pool.Stats().allocation_failures == 2);
}

void ReuseAndHardBudget()
{
    MemoryPool pool({16 * 1024, 16 * 1024});
    std::vector<PoolAllocation> live;
    for (int i = 0; i < 100; ++i)
    {
        auto bytes = pool.TryAllocate(1500);
        if (!bytes) break;
        bytes[1499] = static_cast<uint8_t>(i);
        live.push_back(std::move(bytes));
    }
    CHECK(!live.empty() && live.size() < 100);
    CHECK(pool.Stats().committed_bytes <= pool.Options().max_committed_bytes);
    CHECK(pool.Stats().live_allocations == live.size());
    CHECK(pool.Stats().allocation_failures == 1);
    CHECK(!pool.SetLimits({1, 0}));
    for (size_t i = 0; i < live.size(); ++i) CHECK(live[i][1499] == i);
    live.clear();
    const auto allocations = pool.Stats().system_allocations;
    for (int i = 0; i < 1000; ++i) { auto bytes = pool.Allocate(1500); bytes[0] = 1; }
    CHECK(pool.Stats().system_allocations == allocations);
    CHECK(pool.SetLimits({8 * 1024, 0}));
    CHECK(pool.Stats().committed_bytes == 0);
    auto bytes = pool.Allocate(1024);
    CHECK(pool.Stats().peak_in_use_bytes >= 1500);
    bytes.reset();
    pool.Trim();
    CHECK(pool.Stats().slab_count == 0);
}

void LocalCacheFlushAndPressure()
{
    MemoryPool pool({5000, 5000});
    { auto bytes = pool.Allocate(1); }
    // A different class must reclaim this thread's unused batch to fit.
    auto frame = pool.Allocate(4096);
    CHECK(frame && pool.Stats().allocation_failures == 0);
    frame.reset();
    pool.Trim();
    CHECK(pool.Stats().committed_bytes == 0);

    MemoryPool no_cache({64 * 1024, 0});
    std::thread producer([&] { auto bytes = no_cache.Allocate(20); });
    producer.join();
    CHECK(no_cache.Stats().committed_bytes == 0);
}

void PoolAndProducerCanDieBeforeConsumer()
{
    std::weak_ptr<PoolState> weak;
    std::promise<SharedBuffer> result;
    auto future = result.get_future();
    std::thread producer([&] {
        MemoryPool pool({65536, 65536});
        auto bytes = pool.Allocate(4096);
        weak = bytes.get_deleter().state;
        bytes[123] = 0x42;
        result.set_value(SharedBuffer::Take(std::move(bytes), 4096).Slice(123, 1));
    });
    producer.join();
    auto bytes = future.get();
    CHECK(!weak.expired() && bytes.Data()[0] == 0x42);
    CHECK(bytes.StorageSize() == 4096);
    std::thread consumer([bytes = std::move(bytes)]() mutable { bytes = {}; });
    consumer.join();
    CHECK(weak.expired());
}

void ConcurrentAllocationAndRemoteRelease()
{
    MemoryPool pool({16 * 1024 * 1024, 1024 * 1024});
    constexpr size_t count = 4000;
    std::vector<PoolAllocation> slots(count);
    std::atomic<size_t> produced{0};
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (size_t i = 0; i < count; ++i)
        {
            auto bytes = pool.Allocate((i % 4 + 1) * 1024);
            bytes[0] = static_cast<uint8_t>(i);
            slots[i] = std::move(bytes);
            produced.store(i + 1, std::memory_order_release);
        }
    });
    std::thread consumer([&] {
        for (size_t i = 0; i < count; ++i)
        {
            while (produced.load(std::memory_order_acquire) <= i) std::this_thread::yield();
            if (slots[i][0] != static_cast<uint8_t>(i)) failed = true;
            slots[i].reset();
        }
    });
    std::vector<std::thread> other;
    for (size_t t = 0; t < 4; ++t) other.emplace_back([&, t] {
        for (size_t i = 0; i < count; ++i)
        {
            auto bytes = pool.Allocate(64 << ((i + t) % 9));
            bytes[0] = 7;
        }
    });
    producer.join(); consumer.join();
    for (auto& thread : other) thread.join();
    CHECK(!failed && pool.Stats().live_allocations == 0);
    CHECK(pool.Stats().allocation_failures == 0);
    CHECK(pool.Stats().peak_committed_bytes <= pool.Options().max_committed_bytes);
    pool.Trim();
    CHECK(pool.Stats().committed_bytes == 0);
}

void ContainerAllocatorLifetimeAndFailure()
{
    MemoryPool first({65536, 65536}), second({65536, 65536});
    ByteVector bytes(1024, 0x42, PoolAllocator<uint8_t>(first));
    const auto* allocation = bytes.data();
    ByteVector moved(std::move(bytes));
    bytes.resize(2048, 7); // Moved-from allocator must remain usable.
    CHECK(moved.data() == allocation && moved[1023] == 0x42);
    ByteVector assigned{PoolAllocator<uint8_t>(second)};
    assigned = std::move(moved);
    moved.push_back(9);
    CHECK(assigned.get_allocator() == PoolAllocator<uint8_t>(first));
    CHECK(moved.front() == 9 && bytes[2047] == 7);
    bool failed = false;
    try { assigned.resize(1024 * 1024); } catch (const std::bad_alloc&) { failed = true; }
    CHECK(failed && assigned.size() == 1024 && assigned[1023] == 0x42);
    std::thread releaser([assigned = std::move(assigned)] {});
    releaser.join();
    bytes = ByteVector{}; moved = ByteVector{};
    first.Trim(); second.Trim();
    CHECK(first.Stats().live_allocations == 0 && second.Stats().live_allocations == 0);
    CHECK(first.Stats().committed_bytes == 0);
    MemoryPool::ReleaseThreadCache();
}

void MutablePublicationAndPoolIsolation()
{
    MemoryPool first({65536, 65536}), second({65536, 65536});
    auto write = MutableBuffer::Allocate(4096, first);
    const auto* allocation = write.Data();
    write.Resize(0);
    const uint8_t raw[] = {1, 2, 3, 4};
    write.Append(BufferView(raw, sizeof(raw)));
    auto read = std::move(write).Freeze();
    CHECK(read.Data() == allocation && read.Size() == 4 && read.StorageSize() == 4096);
    CHECK(write.Size() == 0 && write.Capacity() == 0 && write.Data() == nullptr);
    auto copy = SharedBuffer::Copy(read.View(), second);
    CHECK(first.Stats().live_allocations == 1 && second.Stats().live_allocations == 1);
    CHECK(copy == read && copy.Data() != read.Data());
    read = {}; copy = {};
    first.Trim(); second.Trim();
    CHECK(first.Stats().committed_bytes == 0 && second.Stats().committed_bytes == 0);
    MemoryPool empty({0, 0});
    CHECK(SharedBuffer::TryCopy(raw, 4, empty).Empty());
    CHECK(empty.Stats().allocation_failures == 1);
}
}
int main()
{
    try
    {
        SizeClassesAndLargeFrames();
        ReuseAndHardBudget();
        LocalCacheFlushAndPressure();
        PoolAndProducerCanDieBeforeConsumer();
        ConcurrentAllocationAndRemoteRelease();
        ContainerAllocatorLifetimeAndFailure();
        MutablePublicationAndPoolIsolation();
        MemoryPool::ReleaseThreadCache();
        std::cout << "7 memory pool scenarios passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
