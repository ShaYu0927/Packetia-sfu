#include "MetricTypes.h"
#include <algorithm>
#include <chrono>
#include <limits>

namespace monitoring
{
namespace {
constexpr uint64_t kSecond = 1000000000, kWindow = 5 * kSecond;
constexpr auto Bounds()
{
    std::array<uint64_t, 258> bounds{};
    for (size_t i = 0; i <= 16; ++i) bounds[i] = i;
    size_t index = 17;
    for (unsigned exponent = 1; exponent <= 30; ++exponent)
        for (uint64_t part = 1; part <= 8; ++part)
            bounds[index++] = (8 + part) * (uint64_t{1} << exponent);
    bounds.back() = std::numeric_limits<uint64_t>::max();
    return bounds;
}
constexpr auto kBounds = Bounds();
}
uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Histogram::Add(uint64_t ns) noexcept
{
    const auto us = ns / 1000 + (ns % 1000 != 0);
    ++buckets_[std::lower_bound(kBounds.begin(), kBounds.end(), us) - kBounds.begin()];
    ++count_;
    sum_ns_ += ns;
    max_ns_ = std::max(max_ns_, ns);
}
uint64_t Histogram::PercentileUs(unsigned percentile) const noexcept
{
    if (!count_) return 0;
    percentile = std::clamp(percentile, 1U, 100U);
    const auto rank = count_ / 100 * percentile + ((count_ % 100) * percentile + 99) / 100;
    uint64_t seen = 0;
    for (size_t i = 0; i < buckets_.size(); ++i)
        if ((seen += buckets_[i]) >= rank) return std::min(kBounds[i], max_ns_ / 1000 + (max_ns_ % 1000 != 0));
    return 0;
}
void BusyWindow::Add(uint64_t begin, uint64_t end) noexcept
{
    begin = std::max(begin, end > kWindow ? end - kWindow : 0);
    while (begin < end)
    {
        const auto second = begin / kSecond;
        const auto stop = std::min(end, (second + 1) * kSecond);
        const auto slot = second % seconds_.size();
        if (seconds_[slot] != second) { seconds_[slot] = second; busy_ns_[slot] = 0; }
        busy_ns_[slot] += stop - begin;
        begin = stop;
    }
}
uint64_t BusyWindow::Read(uint64_t now) const noexcept
{
    uint64_t total = 0;
    const auto second = now / kSecond;
    for (size_t i = 0; i < seconds_.size(); ++i)
        if (seconds_[i] <= second && second - seconds_[i] < 5) total += busy_ns_[i];
    return total;
}
}
