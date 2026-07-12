#include "H264RtpPayloadParser.h"

namespace media
{

void H264RtpPayloadParser::FillCommon(const RtpView& view, H264ParsedPacket& out)
{
    out.ssrc = view.ssrc;
    out.seq = view.seq;
    out.timestamp = view.ts;
    out.marker = view.marker;
}

void H264RtpPayloadParser::UpdateKeyInfo(H264ParsedPacket& out, H264PayloadUnit& unit)
{
    unit.is_sps = unit.nal_type == static_cast<uint8_t>(H264NalType::Sps);
    unit.is_pps = unit.nal_type == static_cast<uint8_t>(H264NalType::Pps);
    unit.is_idr = unit.nal_type == static_cast<uint8_t>(H264NalType::Idr);
    unit.is_key = H264IsKeyNaluType(unit.nal_type);

    out.has_sps = out.has_sps || unit.is_sps;
    out.has_pps = out.has_pps || unit.is_pps;
    out.has_idr = out.has_idr || unit.is_idr;
    out.has_key_nalu = out.has_key_nalu || unit.is_key;
}

bool H264RtpPayloadParser::Parse(const RtpView& view, H264ParsedPacket& out)
{
    out.Reset();
    FillCommon(view, out);

    if (!view.valid() || !view.payload || view.payload_len == 0)
    {
        out.malformed = true;
        return false;
    }

    const uint8_t nal_header = view.payload[0];
    const uint8_t nal_type = H264GetNalType(nal_header);

    if (nal_type >= 1 && nal_type <= 23)
    {
        return ParseSingleNalu(view, out);
    }

    if (nal_type == static_cast<uint8_t>(H264NalType::StapA))
    {
        return ParseStapA(view, out);
    }

    if (nal_type == static_cast<uint8_t>(H264NalType::FuA))
    {
        return ParseFuA(view, out);
    }

    out.malformed = true;
    return false;
}

bool H264RtpPayloadParser::ParseSingleNalu(const RtpView& view, H264ParsedPacket& out)
{
    H264PayloadUnit unit;
    unit.unit_type = H264PayloadUnitType::SingleNalu;
    unit.nal_type = H264GetNalType(view.payload[0]);
    unit.data.assign(view.payload, view.payload + view.payload_len);

    UpdateKeyInfo(out, unit);

    out.units.emplace_back(std::move(unit));
    out.valid = true;

    // Single NAL packet can be a complete frame by itself,
    // but final frame completion should still be decided by assembler.
    out.begins_frame = false;
    out.ends_frame = view.marker;

    return true;
}

bool H264RtpPayloadParser::ParseStapA(const RtpView& view, H264ParsedPacket& out)
{
    size_t offset = 1;
    while (offset + 2 <= view.payload_len)
    {
        const size_t nalu_size = (static_cast<size_t>(view.payload[offset]) << 8) |
                                 view.payload[offset + 1];
        offset += 2;
        if (nalu_size == 0 || offset + nalu_size > view.payload_len)
        {
            out.malformed = true;
            return false;
        }

        H264PayloadUnit unit;
        unit.unit_type = H264PayloadUnitType::StapANalu;
        unit.nal_type = H264GetNalType(view.payload[offset]);
        if (unit.nal_type == 0 || unit.nal_type >= 24)
        {
            out.malformed = true;
            return false;
        }
        unit.data.assign(view.payload + offset, view.payload + offset + nalu_size);
        UpdateKeyInfo(out, unit);
        out.units.emplace_back(std::move(unit));
        offset += nalu_size;
    }

    if (offset != view.payload_len || out.units.empty())
    {
        out.malformed = true;
        return false;
    }
    out.valid = true;
    out.ends_frame = view.marker;
    return true;
}

bool H264RtpPayloadParser::ParseFuA(const RtpView& view, H264ParsedPacket& out)
{
    if (view.payload_len < 3)
    {
        out.malformed = true;
        return false;
    }

    const uint8_t indicator = view.payload[0];
    const uint8_t header = view.payload[1];
    const bool start = (header & 0x80) != 0;
    const bool end = (header & 0x40) != 0;
    const uint8_t nal_type = header & 0x1F;
    if ((header & 0x20) != 0 || (start && end) || nal_type == 0 || nal_type >= 24)
    {
        out.malformed = true;
        return false;
    }

    H264PayloadUnit unit;
    unit.unit_type = H264PayloadUnitType::FuAFragment;
    unit.nal_type = nal_type;
    unit.fu_start = start;
    unit.fu_end = end;
    unit.reconstructed_nal_header = (indicator & 0xE0) | nal_type;
    unit.data.assign(view.payload + 2, view.payload + view.payload_len);
    UpdateKeyInfo(out, unit);
    out.units.emplace_back(std::move(unit));
    out.valid = true;
    out.begins_frame = start;
    out.ends_frame = end && view.marker;
    return true;
}

} // namespace media
