#include "utils/MediaLatency.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
using namespace media_latency;

uint64_t CountOf(const Snapshot& stats, Counter counter)
{
    return stats.counters[static_cast<std::size_t>(counter)];
}
const Histogram& StageOf(const Snapshot& stats, Stage stage)
{
    return stats.stages[static_cast<std::size_t>(stage)];
}

int main(int argc, char** argv)
try
{
    if (argc > 1)
    {
        unsigned sampled = 0;
        for (unsigned i = 0; i < 100; ++i) if (BeginPacket()) ++sampled;
        CHECK(sampled == (std::string(argv[1]) == "disabled" ? 0U : 25U));
        CHECK(CountOf(TakeSnapshot(), Counter::IngressSampled) == sampled);
        return 0;
    }

    Histogram histogram;
    CHECK(histogram.Count() == 0 && histogram.PercentileUs(99) == 0);
    for (uint64_t us = 1; us <= 100; ++us) histogram.Add(us * 1000);
    CHECK(histogram.Count() == 100);
    CHECK(histogram.MeanUs() == 50.5 && histogram.MaxUs() == 100);
    CHECK(histogram.PercentileUs(50) >= 50 && histogram.PercentileUs(50) <= 56);
    CHECK(histogram.PercentileUs(95) >= 95 && histogram.PercentileUs(95) <= 100);
    CHECK(histogram.PercentileUs(99) >= 99);
    Histogram submicro;
    submicro.Add(0);
    submicro.Add(1);
    CHECK(submicro.PercentileUs(100) == 1);
    CHECK(submicro.MaxUs() == 0.001);
    // Long stalls must not wrap into a fast bucket.
    histogram.Add(3600000000000ULL);
    CHECK(histogram.MaxUs() == 3600000000.0);
    CHECK(histogram.PercentileUs(100) == 3600000000ULL);

    TakeSnapshot();
    PacketTrace trace{1000000, 1100000, 1200000};
    SendTrace send{trace, 1400000};
    std::thread io([send] {
        CHECK(!CurrentPacket() && !CurrentSend());
        SocketSent(send, false, 1700000);
        SocketSent(send, true, 2100000);
        SocketSent({}, true, 2100000);
        SocketSent(send, true, 1300000); // Bad ordering: no unsigned underflow.
        const auto stats = TakeSnapshot();
        CHECK(CountOf(stats, Counter::UdpSent) == 1);
        CHECK(CountOf(stats, Counter::TcpSent) == 1);
        CHECK(StageOf(stats, Stage::UdpTotal).MeanUs() == 700);
        CHECK(StageOf(stats, Stage::UdpSend).MeanUs() == 300);
        CHECK(StageOf(stats, Stage::TcpTotal).MeanUs() == 1100);
        CHECK(StageOf(stats, Stage::TcpSend).MeanUs() == 700);
        CHECK(StageOf(TakeSnapshot(), Stage::TcpTotal).Count() == 0);
    });
    io.join();
    CHECK(CountOf(TakeSnapshot(), Counter::UdpSent) == 0); // Thread isolation.

    CHECK(!CurrentPacket() && !CurrentSend());
    {
        PacketScope packet_scope(trace);
        SendScope normal(false);
        CHECK(CurrentSend().packet.ingress_ns == trace.ingress_ns);
        const auto original = CurrentSend();
        {
            SendScope retransmission(true);
            CHECK(!CurrentSend());
        }
        CHECK(CurrentSend().submitted_ns == original.submitted_ns);
        try
        {
            PacketScope nested({});
            CHECK(!CurrentPacket() && !CurrentSend());
            throw 1;
        }
        catch (int) {}
        CHECK(CurrentPacket().ingress_ns == trace.ingress_ns);
        CHECK(CurrentSend().submitted_ns == original.submitted_ns);
    }
    CHECK(!CurrentPacket() && !CurrentSend());
    Observe(Stage::WorkerQueue, 200, 100);
    CHECK(StageOf(TakeSnapshot(), Stage::WorkerQueue).Count() == 0);
    std::cout << "Media latency histogram, clock boundaries, thread handoff and scopes passed\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
