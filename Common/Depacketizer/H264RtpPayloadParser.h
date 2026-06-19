#ifndef _H264_RTP_PAYLOAD_PARSER_H_
#define _H264_RTP_PAYLOAD_PARSER_H_

#include "H264Payload.h"
#include "Depacketizer.h"

namespace media
{

class H264RtpPayloadParser
{
public:
    H264RtpPayloadParser() = default;
    ~H264RtpPayloadParser() = default;

    bool Parse(const RtpView& view, H264ParsedPacket& out);

private:
    bool ParseSingleNalu(const RtpView& view, H264ParsedPacket& out);
    bool ParseStapA(const RtpView& view, H264ParsedPacket& out);
    bool ParseFuA(const RtpView& view, H264ParsedPacket& out);

    void FillCommon(const RtpView& view, H264ParsedPacket& out);
    void UpdateKeyInfo(H264ParsedPacket& out, H264PayloadUnit& unit);
};

} // namespace media

#endif // _H264_RTP_PAYLOAD_PARSER_H_