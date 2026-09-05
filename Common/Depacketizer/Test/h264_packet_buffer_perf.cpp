#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "H264RtpPayloadParser.h"
#include "H26xPacketBuffer.h"

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::size_t frames = 5000;
    std::size_t packets_per_frame = 100;
    std::size_t payload_size = 1200;
    std::size_t reorder_window = 1;
    std::size_t warmup_frames = 200;
    bool validate_only = false;
};

struct Result
{
    std::size_t packets = 0;
    std::size_t frames = 0;
    std::size_t bytes = 0;
    std::size_t peak_buffered_packets = 0;
    uint64_t checksum = 0;
    double seconds = 0.0;
};

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::size_t ParsePositive(const char* value, const char* option)
{
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value[0] || !end || *end != '\0' || parsed == 0)
        throw std::runtime_error(std::string("invalid value for ") + option);
    return static_cast<std::size_t>(parsed);
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--help")
        {
            std::cout << "Usage: h264_packet_buffer_perf [--validate-only] [--frames N] "
                         "[--packets-per-frame N] [--payload-size N] "
                         "[--reorder-window N] [--warmup N]\n";
            std::exit(0);
        }
        if (option == "--validate-only")
        {
            options.validate_only = true;
            continue;
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + option);
        const char* value = argv[++i];
        if (option == "--frames") options.frames = ParsePositive(value, "--frames");
        else if (option == "--packets-per-frame") options.packets_per_frame = ParsePositive(value, "--packets-per-frame");
        else if (option == "--payload-size") options.payload_size = ParsePositive(value, "--payload-size");
        else if (option == "--reorder-window") options.reorder_window = ParsePositive(value, "--reorder-window");
        else if (option == "--warmup") options.warmup_frames = ParsePositive(value, "--warmup");
        else throw std::runtime_error("unknown option: " + option);
    }
    Require(options.packets_per_frame >= 2, "--packets-per-frame must be at least 2");
    Require(options.packets_per_frame <= 512, "--packets-per-frame exceeds buffer frame limit 512");
    Require(options.payload_size >= 3, "--payload-size must be at least 3");
    Require(options.reorder_window <= options.packets_per_frame,
            "--reorder-window cannot exceed packets per frame");
    return options;
}

std::vector<uint8_t> FuA(std::size_t index, std::size_t packet_count,
                         std::size_t payload_size)
{
    std::vector<uint8_t> payload(payload_size, 0x5A);
    payload[0] = 0x7C;
    payload[1] = 0x05;
    if (index == 0) payload[1] |= 0x80;
    if (index + 1 == packet_count) payload[1] |= 0x40;
    return payload;
}

RtpView View(uint16_t sequence, uint32_t timestamp, bool marker,
             const std::vector<uint8_t>& payload)
{
    RtpView view;
    view.ssrc = 0x11223344;
    view.seq = sequence;
    view.ts = timestamp;
    view.marker = marker;
    view.payload = payload.data();
    view.payload_len = payload.size();
    return view;
}

media::H264ParsedPacket Parse(media::H264RtpPayloadParser& parser,
                              uint16_t sequence, uint32_t timestamp, bool marker,
                              const std::vector<uint8_t>& payload)
{
    media::H264ParsedPacket packet;
    Require(parser.Parse(View(sequence, timestamp, marker, payload), packet),
            "parser rejected a valid synthetic packet");
    return packet;
}

void ValidateSingleNalu()
{
    media::H264RtpPayloadParser parser;
    media::H264PacketBuffer buffer;
    auto result = buffer.InsertPacket(Parse(parser, 10, 1000, true, {0x65, 0xAA, 0xBB}));
    Require(result.frames.size() == 1, "single NALU did not produce one frame");
    Require(result.frames[0].complete && result.frames[0].has_idr, "single NALU metadata is invalid");
    Require(result.frames[0].nalus[0] == std::vector<uint8_t>({0x65, 0xAA, 0xBB}),
            "single NALU bytes differ");
}

void ValidateStapA()
{
    media::H264RtpPayloadParser parser;
    media::H264PacketBuffer buffer;
    const std::vector<uint8_t> payload = {
        0x78, 0x00, 0x02, 0x67, 0x11, 0x00, 0x02, 0x68, 0x22,
        0x00, 0x03, 0x65, 0x33, 0x44};
    auto result = buffer.InsertPacket(Parse(parser, 20, 2000, true, payload));
    Require(result.frames.size() == 1, "STAP-A did not produce one frame");
    Require(result.frames[0].nalus.size() == 3, "STAP-A NALU count differs");
    Require(result.frames[0].has_sps && result.frames[0].has_pps && result.frames[0].has_idr,
            "STAP-A keyframe metadata is invalid");
}

void ValidateReorderRetransmitAndWrap()
{
    media::H264RtpPayloadParser parser;
    media::H264PacketBuffer buffer;
    std::vector<std::vector<uint8_t>> payloads;
    for (std::size_t i = 0; i < 4; ++i) payloads.emplace_back(FuA(i, 4, 16));
    const std::size_t arrival[] = {0, 3, 2, 1};
    std::size_t emitted = 0;
    media::H264AccessUnit frame;
    for (std::size_t index : arrival)
    {
        auto result = buffer.InsertPacket(Parse(parser,
            static_cast<uint16_t>(65534 + index), 3000, index == 3, payloads[index]));
        emitted += result.frames.size();
        if (!result.frames.empty()) frame = std::move(result.frames.front());
    }
    Require(emitted == 1, "reordered FU-A did not produce exactly one frame");
    Require(frame.complete && frame.first_seq == 65534 && frame.last_seq == 1,
            "sequence-wrap frame metadata is invalid");
    Require(frame.nalus.size() == 1 && frame.nalus[0].size() == 1 + 4 * 14,
            "FU-A reconstructed NALU size differs");
}

void ValidateDuplicateAndMissingPacket()
{
    media::H264RtpPayloadParser parser;
    media::H264PacketBuffer buffer;
    const auto start = FuA(0, 3, 8);
    const auto end = FuA(2, 3, 8);
    Require(buffer.InsertPacket(Parse(parser, 100, 4000, false, start)).frames.empty(),
            "FU-A start produced a frame early");
    auto duplicate = buffer.InsertPacket(Parse(parser, 100, 4000, false, start));
    Require((duplicate.duplicate || duplicate.late) && duplicate.frames.empty(),
            "duplicate packet was not rejected");
    Require(buffer.InsertPacket(Parse(parser, 102, 4000, true, end)).frames.empty(),
            "frame with a missing packet was emitted");
}

void Validate()
{
    ValidateSingleNalu();
    ValidateStapA();
    ValidateReorderRetransmitAndWrap();
    ValidateDuplicateAndMissingPacket();
}

std::vector<std::size_t> ArrivalOrder(std::size_t packet_count, std::size_t window)
{
    std::vector<std::size_t> order(packet_count);
    std::iota(order.begin(), order.end(), 0);
    if (window <= 1) return order;
    for (std::size_t begin = 1; begin < packet_count; begin += window)
    {
        const std::size_t end = std::min(packet_count, begin + window);
        std::reverse(order.begin() + static_cast<std::ptrdiff_t>(begin),
                     order.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return order;
}

Result Benchmark(std::size_t frame_count, const Options& options)
{
    media::H264RtpPayloadParser parser;
    media::H264PacketBuffer buffer;
    std::vector<std::vector<uint8_t>> payloads;
    payloads.reserve(options.packets_per_frame);
    for (std::size_t i = 0; i < options.packets_per_frame; ++i)
        payloads.emplace_back(FuA(i, options.packets_per_frame, options.payload_size));
    const auto order = ArrivalOrder(options.packets_per_frame, options.reorder_window);

    Result result;
    uint16_t first_sequence = 0;
    uint32_t timestamp = 0;
    const auto started = Clock::now();
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index)
    {
        timestamp += 3000;
        for (std::size_t packet_index : order)
        {
            const uint16_t sequence = static_cast<uint16_t>(first_sequence + packet_index);
            auto packet = Parse(parser, sequence, timestamp,
                                packet_index + 1 == options.packets_per_frame,
                                payloads[packet_index]);
            auto inserted = buffer.InsertPacket(std::move(packet));
            ++result.packets;
            result.bytes += options.payload_size;
            result.peak_buffered_packets = std::max(result.peak_buffered_packets,
                                                     buffer.BufferedPacketCount());
            for (const auto& frame : inserted.frames)
            {
                ++result.frames;
                result.checksum += frame.SizeBytes() + frame.timestamp;
            }
        }
        first_sequence = static_cast<uint16_t>(first_sequence + options.packets_per_frame);
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    Require(result.frames == frame_count, "benchmark output frame count differs from input");
    Require(buffer.BufferedPacketCount() == 0, "benchmark left packets buffered after complete input");
    return result;
}

void Print(const Result& result)
{
    const double packet_rate = result.packets / result.seconds;
    const double frame_rate = result.frames / result.seconds;
    const double mib_rate = result.bytes / result.seconds / (1024.0 * 1024.0);
    const double ns_per_packet = result.seconds * 1e9 / result.packets;
    std::cout << std::fixed << std::setprecision(2)
              << "packets=" << result.packets
              << " frames=" << result.frames
              << " seconds=" << result.seconds
              << " packets/s=" << packet_rate
              << " frames/s=" << frame_rate
              << " MiB/s=" << mib_rate
              << " ns/packet=" << ns_per_packet
              << " peak_buffered_packets=" << result.peak_buffered_packets
              << " checksum=" << result.checksum << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = ParseOptions(argc, argv);
        Validate();
        std::cout << "validation PASS: single-nalu, stap-a, fu-a, reorder, retransmit, "
                     "duplicate, missing-packet, sequence-wrap\n";
        if (options.validate_only) return 0;

        Benchmark(options.warmup_frames, options);
        std::cout << "config frames=" << options.frames
                  << " packets_per_frame=" << options.packets_per_frame
                  << " payload_size=" << options.payload_size
                  << " reorder_window=" << options.reorder_window
                  << " warmup_frames=" << options.warmup_frames << '\n';
        Print(Benchmark(options.frames, options));
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "h264_packet_buffer_perf: " << error.what() << '\n';
        return 1;
    }
}
