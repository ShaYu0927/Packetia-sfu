#include "H264RtpPayloadParser.h"
#include "logger.h"

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

    LOG_ERROR("[H264Parser] unsupported nal type",
              " type=", static_cast<int>(nal_type),
              " seq=", view.seq,
              " ts=", view.ts,
              " ssrc=", view.ssrc,
              " payload_len=", view.payload_len);

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
    out.begins_frame = true;
    out.ends_frame = view.marker;

    return true;
}

bool H264RtpPayloadParser::ParseStapA(const RtpView& view, H264ParsedPacket& out)
{
    // TODO:
    // STAP-A format:
    // [STAP-A header][NAL size][NAL data][NAL size][NAL data]...
    //
    // Today only define framework.
    // Implement in next step.

    (void)view;
    (void)out;

    return false;
}

bool H264RtpPayloadParser::ParseFuA(const RtpView& view, H264ParsedPacket& out)
{
    // TODO:
    // FU-A format:
    // [FU indicator][FU header][fragment payload]
    //
    // Parser should only parse FU metadata and fragment payload.
    // Full NAL reassembly should be done by RtpFrameAssembler.

    (void)view;
    (void)out;

    return false;
}

} // namespace media

