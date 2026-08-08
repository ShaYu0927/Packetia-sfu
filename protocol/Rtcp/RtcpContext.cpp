#include "RtcpContext.h"

#include "RtcpFeedback.h"
#include "RtcpHealper.h"
#include "RtcpReciver.h"
#include "utils.h"

namespace rtcpx
{

/* RTCP packet types */
constexpr uint8_t kRtcpSr       = 200;
constexpr uint8_t kRtcpRr       = 201;
constexpr uint8_t kRtcpSdes     = 202;
constexpr uint8_t kRtcpBye      = 203;
constexpr uint8_t kRtcpApp      = 204;
constexpr uint8_t kRtcpRtpfb    = 205;
constexpr uint8_t kRtcpPsfb     = 206;
constexpr uint8_t kRtcpXr       = 207;


/* RTPFB formats */
constexpr uint8_t kRtpFbNack        = 1;
constexpr uint8_t kRtpFbTransportCc = 15;

/* PSFB formats */
constexpr uint8_t kPsFbPli = 1;
constexpr uint8_t kPsFbFir = 4;

constexpr size_t kCommonHeaderSize = 4;
constexpr size_t kReportBlockSize = 24;

namespace
{
class RtcpSenderImpl final : public IRtcpSender
{
public:
    bool GenerateRtcp(uint32_t media_ssrc, std::vector<uint8_t>& out) override
    {
        // With no sender/reception statistics supplied by this interface, the
        // only truthful periodic report is an empty Receiver Report. The
        // argument is used as the local RTCP reporter SSRC.
        out.assign(8, 0);
        out[0] = static_cast<uint8_t>(2U << 6); // V=2, P=0, RC=0.
        out[1] = kRtcpRr;
        utils::Utils::WriteUint16BE(out.data() + 2, 1); // 2 words - 1.
        utils::Utils::WriteUint32BE(out.data() + 4, media_ssrc);
        return true;
    }

    std::vector<uint8_t> BuildPli(uint32_t sender_ssrc, uint32_t media_ssrc) override
    {
        return RtcpFeedback::BuildPli(sender_ssrc, media_ssrc);
    }
};
}

bool InspectRtcpPacket(const uint8_t* data, size_t len, RtcpPacketInfo* out)
{
    if (out)
    {
        *out = RtcpPacketInfo{};
    }
    if (!data || len < kCommonHeaderSize)
    {
        return false;
    }

    RtcpPacketInfo info{};
    size_t offset = 0;
    bool parsed_any = false;
    while (offset < len)
    {
        RtcpHeader header;
        if (!header.Parse(data + offset, len - offset))
        {
            return false;
        }

        const uint8_t* packet = header.Data();
        const size_t packet_len = header.ContentSize();
        const uint8_t pt = header.PacketType();
        const uint8_t count_or_fmt = header.CountOrFmt();

        if (!parsed_any)
        {
            info.first_packet_type = pt;
            info.first_count_or_fmt = count_or_fmt;
        }

        if ((pt == kRtcpSr || pt == kRtcpRr || pt == kRtcpRtpfb || pt == kRtcpPsfb) &&
            packet_len >= 8)
        {
            info.sender_ssrc = utils::Utils::ReadUint32BE(packet + 4);
            info.has_sender_ssrc = true;
        }

        // Prefer an explicit feedback target or report-block SSRC. For SR
        // without report blocks, the sender SSRC is the media source itself.
        if ((pt == kRtcpRtpfb || pt == kRtcpPsfb) && packet_len >= 12)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(packet + 8);
            info.has_media_ssrc = true;
            if (pt == kRtcpPsfb && count_or_fmt == kPsFbFir &&
                info.media_ssrc == 0 && packet_len >= 20)
            {
                info.media_ssrc = utils::Utils::ReadUint32BE(packet + 12);
            }
        }
        else if (pt == kRtcpRr && count_or_fmt > 0 && packet_len >= 8 + kReportBlockSize)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(packet + 8);
            info.has_media_ssrc = true;
        }
        else if (pt == kRtcpSr && count_or_fmt > 0 && packet_len >= 28 + kReportBlockSize)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(packet + 28);
            info.has_media_ssrc = true;
        }
        else if (pt == kRtcpSr && packet_len >= 8)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(packet + 4);
            info.has_media_ssrc = true;
        }
        else if (pt == kRtcpBye && count_or_fmt > 0 && packet_len >= 8)
        {
            info.sender_ssrc = utils::Utils::ReadUint32BE(packet + 4);
            info.media_ssrc = info.sender_ssrc;
            info.has_sender_ssrc = true;
            info.has_media_ssrc = true;
        }

        offset += header.PacketSize();
        parsed_any = true;
    }

    if (!parsed_any || offset != len)
    {
        return false;
    }
    info.valid = true;
    if (out)
    {
        *out = info;
    }
    return true;
}

std::unique_ptr<IRtcpReceiver> CreateRtcpReceiver(IRtcpObserver* observer)
{
    return std::make_unique<RtcpReceiverImpl>(observer);
}

std::unique_ptr<IRtcpSender> CreateRtcpSender()
{
    return std::make_unique<RtcpSenderImpl>();
}



}
