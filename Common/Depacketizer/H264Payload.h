#ifndef _H264_PAYLOAD_H_
#define _H264_PAYLOAD_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media
{

enum class H264PayloadUnitType { Unknown = 0, SingleNalu, StapANalu, FuAFragment };
enum class H264NalType : uint8_t
{
    Unspecified = 0, NonIdr = 1, Dpa = 2, Dpb = 3, Dpc = 4, Idr = 5,
    Sei = 6, Sps = 7, Pps = 8, Aud = 9, StapA = 24, FuA = 28
};

inline uint8_t H264GetNalType(uint8_t nal_header) { return nal_header & 0x1F; }
inline bool H264IsKeyNaluType(uint8_t type)
{
    return type == static_cast<uint8_t>(H264NalType::Idr) ||
           type == static_cast<uint8_t>(H264NalType::Sps) ||
           type == static_cast<uint8_t>(H264NalType::Pps);
}

struct H264PayloadUnit
{
    H264PayloadUnitType unit_type = H264PayloadUnitType::Unknown;
    uint8_t nal_type = 0;
    std::vector<uint8_t> data;
    bool fu_start = false;
    bool fu_end = false;
    uint8_t reconstructed_nal_header = 0;
    bool is_sps = false;
    bool is_pps = false;
    bool is_idr = false;
    bool is_key = false;
};

struct H264ParsedPacket
{
    bool valid = false;
    bool malformed = false;
    uint32_t ssrc = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    bool marker = false;
    bool begins_frame = false;
    bool ends_frame = false;
    bool has_key_nalu = false;
    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;
    std::vector<H264PayloadUnit> units;
    void Reset() { *this = H264ParsedPacket{}; }
};

struct H264AccessUnit
{
    uint32_t ssrc = 0;
    uint32_t timestamp = 0;
    uint16_t first_seq = 0;
    uint16_t last_seq = 0;
    bool complete = false;
    bool broken = false;
    bool marker = false;
    bool keyframe = false;
    bool has_sps = false;
    bool has_pps = false;
    bool has_idr = false;
    std::vector<std::vector<uint8_t>> nalus;

    size_t SizeBytes() const
    {
        size_t total = 0;
        for (const auto& nalu : nalus) total += nalu.size();
        return total;
    }

    std::vector<uint8_t> ToAnnexB() const
    {
        static const uint8_t start_code[4] = {0, 0, 0, 1};
        std::vector<uint8_t> out;
        for (const auto& nalu : nalus)
        {
            out.insert(out.end(), start_code, start_code + 4);
            out.insert(out.end(), nalu.begin(), nalu.end());
        }
        return out;
    }
};

} // namespace media
#endif // _H264_PAYLOAD_H_
