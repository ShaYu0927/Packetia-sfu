#include "H264ParameterSetTracker.h"

#include <limits>

namespace media
{
namespace
{

std::vector<uint8_t> NaluToRbsp(const uint8_t* nalu, size_t size)
{
    std::vector<uint8_t> rbsp;
    if (!nalu || size < 2)
        return rbsp;
    rbsp.reserve(size - 1);
    int zero_count = 0;
    for (size_t i = 1; i < size; ++i)
    {
        const uint8_t value = nalu[i];
        if (zero_count >= 2 && value == 0x03)
        {
            zero_count = 0;
            continue;
        }
        rbsp.push_back(value);
        zero_count = value == 0 ? zero_count + 1 : 0;
    }
    return rbsp;
}

class BitReader
{
public:
    explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

    bool ReadBits(size_t count, uint32_t& value)
    {
        if (count > 32 || bit_offset_ + count > data_.size() * 8)
            return false;
        value = 0;
        for (size_t i = 0; i < count; ++i)
        {
            value = (value << 1) |
                    ((data_[bit_offset_ / 8] >> (7 - bit_offset_ % 8)) & 1);
            ++bit_offset_;
        }
        return true;
    }

    bool ReadBit(bool& value)
    {
        uint32_t bit = 0;
        if (!ReadBits(1, bit)) return false;
        value = bit != 0;
        return true;
    }

    bool ReadUE(uint32_t& value)
    {
        size_t zeros = 0;
        bool bit = false;
        while (true)
        {
            if (!ReadBit(bit)) return false;
            if (bit) break;
            if (++zeros > 31) return false;
        }
        uint32_t suffix = 0;
        if (zeros && !ReadBits(zeros, suffix)) return false;
        value = static_cast<uint32_t>((uint64_t{1} << zeros) - 1 + suffix);
        return true;
    }

    bool ReadSE(int32_t& value)
    {
        uint32_t code = 0;
        if (!ReadUE(code)) return false;
        value = (code & 1) ? static_cast<int32_t>((code + 1) / 2)
                           : -static_cast<int32_t>(code / 2);
        return true;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t bit_offset_ = 0;
};

bool SkipScalingList(BitReader& reader, size_t size)
{
    int last_scale = 8;
    int next_scale = 8;
    for (size_t i = 0; i < size; ++i)
    {
        if (next_scale != 0)
        {
            int32_t delta = 0;
            if (!reader.ReadSE(delta)) return false;
            next_scale = (last_scale + delta + 256) % 256;
        }
        last_scale = next_scale == 0 ? last_scale : next_scale;
    }
    return true;
}

bool IsHighProfile(uint32_t profile)
{
    switch (profile)
    {
    case 44: case 83: case 86: case 100: case 110: case 118: case 122:
    case 128: case 134: case 135: case 138: case 139: case 144: case 244:
        return true;
    default:
        return false;
    }
}

bool ParseSps(const uint8_t* nalu, size_t size, SpsInfo& info)
{
    if (!nalu || size < 4 || H264GetNalType(nalu[0]) != 7)
        return false;
    const auto rbsp = NaluToRbsp(nalu, size);
    BitReader reader(rbsp);
    uint32_t profile = 0, ignored = 0, level = 0;
    if (!reader.ReadBits(8, profile) || !reader.ReadBits(8, ignored) ||
        !reader.ReadBits(8, level) || !reader.ReadUE(info.sps_id))
        return false;

    uint32_t chroma_format = 1;
    bool separate_colour_plane = false;
    if (IsHighProfile(profile))
    {
        if (!reader.ReadUE(chroma_format) || chroma_format > 3) return false;
        if (chroma_format == 3 && !reader.ReadBit(separate_colour_plane)) return false;
        uint32_t bit_depth_luma = 0, bit_depth_chroma = 0;
        bool transform_bypass = false, scaling_present = false;
        if (!reader.ReadUE(bit_depth_luma) || !reader.ReadUE(bit_depth_chroma) ||
            !reader.ReadBit(transform_bypass) || !reader.ReadBit(scaling_present))
            return false;
        if (scaling_present)
        {
            const size_t count = chroma_format == 3 ? 12 : 8;
            for (size_t i = 0; i < count; ++i)
            {
                bool present = false;
                if (!reader.ReadBit(present)) return false;
                if (present && !SkipScalingList(reader, i < 6 ? 16 : 64)) return false;
            }
        }
    }

    uint32_t log2_frame_num = 0, poc_type = 0;
    if (!reader.ReadUE(log2_frame_num) || !reader.ReadUE(poc_type)) return false;
    if (poc_type == 0)
    {
        uint32_t value = 0;
        if (!reader.ReadUE(value)) return false;
    }
    else if (poc_type == 1)
    {
        bool delta_always_zero = false;
        int32_t offset = 0;
        uint32_t cycle = 0;
        if (!reader.ReadBit(delta_always_zero) || !reader.ReadSE(offset) ||
            !reader.ReadSE(offset) || !reader.ReadUE(cycle)) return false;
        for (uint32_t i = 0; i < cycle; ++i)
            if (!reader.ReadSE(offset)) return false;
    }
    else if (poc_type > 2) return false;

    uint32_t max_refs = 0, width_mbs = 0, height_map_units = 0;
    bool gaps = false, frame_mbs_only = false, direct_8x8 = false, crop = false;
    if (!reader.ReadUE(max_refs) || !reader.ReadBit(gaps) ||
        !reader.ReadUE(width_mbs) || !reader.ReadUE(height_map_units) ||
        !reader.ReadBit(frame_mbs_only)) return false;
    if (!frame_mbs_only)
    {
        bool mb_adaptive = false;
        if (!reader.ReadBit(mb_adaptive)) return false;
    }
    if (!reader.ReadBit(direct_8x8) || !reader.ReadBit(crop)) return false;

    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (crop && (!reader.ReadUE(left) || !reader.ReadUE(right) ||
                 !reader.ReadUE(top) || !reader.ReadUE(bottom))) return false;

    const uint32_t chroma_array = separate_colour_plane ? 0 : chroma_format;
    const uint32_t sub_width = chroma_array == 1 || chroma_array == 2 ? 2 : 1;
    const uint32_t sub_height = chroma_array == 1 ? 2 : 1;
    const uint32_t crop_x = chroma_array == 0 ? 1 : sub_width;
    const uint32_t crop_y = (chroma_array == 0 ? 1 : sub_height) *
                            (2 - static_cast<uint32_t>(frame_mbs_only));
    const uint64_t coded_width = (uint64_t{width_mbs} + 1) * 16;
    const uint64_t coded_height = (uint64_t{2} - frame_mbs_only) *
                                  (uint64_t{height_map_units} + 1) * 16;
    const uint64_t crop_width = uint64_t{crop_x} * (left + right);
    const uint64_t crop_height = uint64_t{crop_y} * (top + bottom);
    if (crop_width >= coded_width || crop_height >= coded_height ||
        coded_width > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        coded_height > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    info.width = static_cast<int>(coded_width - crop_width);
    info.height = static_cast<int>(coded_height - crop_height);
    info.payload.assign(nalu, nalu + size);
    return true;
}

bool ParsePps(const uint8_t* nalu, size_t size, PpsInfo& info)
{
    if (!nalu || size < 2 || H264GetNalType(nalu[0]) != 8)
        return false;
    const auto rbsp = NaluToRbsp(nalu, size);
    BitReader reader(rbsp);
    if (!reader.ReadUE(info.pps_id) || !reader.ReadUE(info.sps_id))
        return false;
    info.payload.assign(nalu, nalu + size);
    return true;
}

} // namespace

bool H264ParameterSetTracker::UpdateAccessUnit(const H264AccessUnit& frame)
{
    bool updated = false;
    for (const auto& nalu : frame.nalus)
    {
        if (nalu.empty()) continue;
        const uint8_t type = H264GetNalType(nalu[0]);
        if (type == 7) updated = UpdateSps(nalu.data(), nalu.size()) || updated;
        else if (type == 8) updated = UpdatePps(nalu.data(), nalu.size()) || updated;
    }
    return updated;
}

bool H264ParameterSetTracker::UpdateSps(const uint8_t* nalu, size_t size)
{
    SpsInfo info;
    if (!ParseSps(nalu, size, info)) return false;
    latest_sps_id_ = info.sps_id;
    has_latest_sps_ = true;
    sps_by_id_[info.sps_id] = std::move(info);
    return true;
}

bool H264ParameterSetTracker::UpdatePps(const uint8_t* nalu, size_t size)
{
    PpsInfo info;
    if (!ParsePps(nalu, size, info)) return false;
    latest_pps_id_ = info.pps_id;
    has_latest_pps_ = true;
    pps_by_id_[info.pps_id] = std::move(info);
    return true;
}

const SpsInfo* H264ParameterSetTracker::GetSps(uint32_t id) const
{
    const auto it = sps_by_id_.find(id);
    return it == sps_by_id_.end() ? nullptr : &it->second;
}

const PpsInfo* H264ParameterSetTracker::GetPps(uint32_t id) const
{
    const auto it = pps_by_id_.find(id);
    return it == pps_by_id_.end() ? nullptr : &it->second;
}

const SpsInfo* H264ParameterSetTracker::LatestSps() const
{
    return has_latest_sps_ ? GetSps(latest_sps_id_) : nullptr;
}

const PpsInfo* H264ParameterSetTracker::LatestPps() const
{
    return has_latest_pps_ ? GetPps(latest_pps_id_) : nullptr;
}

void H264ParameterSetTracker::Reset()
{
    sps_by_id_.clear(); pps_by_id_.clear();
    has_latest_sps_ = false; has_latest_pps_ = false;
}

} // namespace media
