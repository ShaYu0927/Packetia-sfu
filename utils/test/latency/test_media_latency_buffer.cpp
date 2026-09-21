#include "network/BufferWrite.h"
#include "utils/MediaLatency.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
using namespace media_latency;

// Interpose send only in this test executable. Exercise the production write
// queue deterministically, including partial writes, EAGAIN and close/discard.
static std::deque<int> results;
extern "C" ssize_t send(int, const void*, size_t size, int)
{
    if (results.empty()) std::abort();
    const int result = results.front();
    results.pop_front();
    if (result < 0) { errno = EAGAIN; return -1; }
    return static_cast<ssize_t>(std::min(size, static_cast<size_t>(result)));
}

uint64_t CountOf(const Snapshot& stats, Counter counter)
{
    return stats.counters[static_cast<std::size_t>(counter)];
}

int main()
try
{
    const auto now = NowNs();
    const PacketTrace trace{now - 3000000, now - 2000000, now - 1000000};
    BufferWirte buffer;
    {
        PacketScope packet(trace);
        SendScope send_scope(false);
        CHECK(buffer.Append("test", 4));
    }
    CHECK(CountOf(TakeSnapshot(), Counter::TcpSent) == 0); // Enqueue is not send.
    results = {2, -1};
    const auto partial = buffer.Send(-1);
#if defined(__linux__) || defined(__linux)
    CHECK(partial == 0);
#else
    (void)partial; // Other production socket backends do not yet handle EAGAIN.
#endif
    CHECK(buffer.QueuedBytes() == 2);
    CHECK(CountOf(TakeSnapshot(), Counter::TcpSent) == 0);
    results = {2};
    CHECK(buffer.Send(-1) == 0 && buffer.IsEmpty());
    const auto completed = TakeSnapshot();
    CHECK(CountOf(completed, Counter::TcpSent) == 1);
    CHECK(completed.stages[static_cast<std::size_t>(Stage::TcpTotal)].MaxUs() >= 3000);
    CHECK(buffer.Send(-1) == 0);
    CHECK(CountOf(TakeSnapshot(), Counter::TcpSent) == 0); // Exactly once.

    {
        BufferWirte abandoned;
        PacketScope packet(trace);
        SendScope send_scope(false);
        CHECK(abandoned.Append("test", 4));
        results = {1, -1};
        abandoned.Send(-1);
        CHECK(abandoned.QueuedBytes() == 3);
    }
    const auto closed = TakeSnapshot();
    CHECK(CountOf(closed, Counter::TcpAbandoned) == 1);
    CHECK(CountOf(closed, Counter::TcpSent) == 0);
    {
        PacketScope packet(trace);
        SendScope retransmission(true);
        CHECK(buffer.Append("rtx!", 4));
    }
    CHECK(buffer.Append("RTSP", 4));
    results = {4, 4};
    CHECK(buffer.Send(-1) == 0);
    CHECK(CountOf(TakeSnapshot(), Counter::TcpSent) == 0);
    std::cout << "TCP latency waits for full write, excludes retries and counts abandoned sends\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
