#include "RtpRtx.h"
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(value) do { if (!(value)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " + #value); } while (false)

int main()
{
    try {
        // Marker, one CSRC and an RFC 8285 extension, followed by H264 payload.
        const std::vector<uint8_t> original{0x91, 0xe0, 0xff, 0xff, 0, 0, 0, 77,
            0, 0, 0, 11, 0, 0, 0, 99, 0xbe, 0xde, 0, 1, 0x10, 'v', 0, 0, 0x65, 0xaa};
        auto packet = original;
        CHECK(rtsp::EncapsulateRtx(packet, 103, 22, 0));
        CHECK(packet[1] == 0xe7 && packet[2] == 0 && packet[3] == 0 && packet[11] == 22);
        CHECK(packet[24] == 0xff && packet[25] == 0xff && packet[26] == 0x65);
        CHECK(packet[7] == 77 && packet[15] == 99 && packet[21] == 'v');
        CHECK(rtsp::DecapsulateRtx(packet, 96, 11));
        CHECK(packet == original);
        auto padded = original;
        padded[0] |= 0x20;
        padded.insert(padded.end(), {0, 0, 0, 4});
        CHECK(rtsp::EncapsulateRtx(padded, 103, 22, 65535));
        CHECK(!(padded[0] & 0x20) && padded.size() == original.size() + 2);
        padded[0] |= 0x20;
        padded.insert(padded.end(), {0, 0, 0, 4});
        CHECK(rtsp::DecapsulateRtx(padded, 96, 11));
        CHECK(padded == original);
        for (size_t length = 0; length <= 26; ++length) {
            auto truncated = original;
            truncated.resize(length);
            const auto before = truncated;
            // Header must end before an OSN plus at least one payload byte.
            CHECK(!rtsp::DecapsulateRtx(truncated, 96, 11));
            CHECK(truncated == before);
        }
        auto bad = original;
        bad[19] = 255; // Extension envelope extends beyond the packet.
        CHECK(!rtsp::EncapsulateRtx(bad, 103, 22, 0));
        bad = original;
        bad[0] |= 0x20;
        bad.back() = 255; // Padding cannot consume header/extension bytes.
        CHECK(!rtsp::DecapsulateRtx(bad, 96, 11));
        CHECK(!rtsp::EncapsulateRtx(packet, 128, 22, 0));
        CHECK(!rtsp::DecapsulateRtx(packet, 96, 0));
        std::cout << "RTP RTX packet tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
