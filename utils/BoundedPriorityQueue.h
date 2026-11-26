#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace utils
{
template<class T>
class BoundedPriorityQueue
{
public:
    enum class Priority { Normal, High };

    BoundedPriorityQueue(std::size_t normal_capacity, std::size_t high_capacity)
        : normal_{{}, normal_capacity}, high_{{}, high_capacity} {}

    template<class Value>
    bool TryPush(Priority priority, Value&& value)
    {
        auto& bucket = priority == Priority::High ? high_ : normal_;
        if (bucket.items.size() >= bucket.capacity) return false;
        bucket.items.emplace_back(std::forward<Value>(value));
        return true;
    }

    std::optional<T> TryPop()
    {
        auto& items = high_.items.empty() ? normal_.items : high_.items;
        if (items.empty()) return std::nullopt;

        std::optional<T> value(std::in_place, std::move(items.front()));
        items.pop_front();
        return value;
    }

    bool Empty() const noexcept { return normal_.items.empty() && high_.items.empty(); }

private:
    struct Bucket
    {
        std::deque<T> items;
        std::size_t capacity;
    };

    Bucket normal_;
    Bucket high_;
};
}
