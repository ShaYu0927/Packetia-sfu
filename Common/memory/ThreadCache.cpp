#include "ThreadCache.h"
#include "CentralCache.h"
#include <utility>

namespace common
{
ThreadCache& GetThreadCache()
{
    thread_local ThreadCache cache;
    return cache;
}
ThreadCache::~ThreadCache() { Flush(); }

void ThreadCache::Flush() noexcept
{
    if (!state_) return;
    for (auto& list : lists_)
        if (list) state_->central.ReleaseBatch(std::exchange(list, nullptr));
    state_.reset();
}
void ThreadCache::FlushFor(const std::shared_ptr<PoolState>& state) noexcept
{
    if (state_ == state) Flush();
}
BlockHeader* ThreadCache::Allocate(const std::shared_ptr<PoolState>& state, std::size_t size) noexcept
{
    if (state_ != state) { Flush(); state_ = state; }
    const auto index = GetSizeClassIndex(size);
    auto& head = lists_[index];
    if (!head) head = state_->central.FetchBatch(index, GetBatchCount(index));
    if (!head)
    {
        // Our own idle classes must not cause a false budget exhaustion.
        Flush();
        state_ = state;
        state_->central.Trim();
        head = state_->central.FetchBatch(index, 1);
    }
    if (!head) return nullptr;
    auto* block = head;
    head = head->next;
    block->next = nullptr;
    state_->central.Activate(block, size);
    return block;
}
} // namespace common
