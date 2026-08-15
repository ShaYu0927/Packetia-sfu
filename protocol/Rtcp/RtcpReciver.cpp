#include "RtcpReciver.h"
#include "BufferRead.h"
#include "utils.h"
#include "RtcpHealper.h"
#include "logger.h"
#include <vector>

namespace
{
uint32_t ReadUint24BE(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

int32_t ReadInt24BE(const uint8_t* p)
{
    uint32_t value = ReadUint24BE(p);
    if ((value & 0x00800000U) != 0)
    {
        value |= 0xFF000000U;
    }
    return static_cast<int32_t>(value);
}

bool ParseTransportFeedback(const uint8_t* p, size_t len, rtcpx::TransportFeedbackReport& report)
{
    if (!p || len < 20)
    {
        return false;
    }

    report = rtcpx::TransportFeedbackReport{};
    report.sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
    report.media_ssrc = utils::Utils::ReadUint32BE(p + 8);
    report.base_sequence = utils::Utils::ReadUint16BE(p + 12);
    report.packet_status_count = utils::Utils::ReadUint16BE(p + 14);
    report.reference_time_ms = static_cast<uint64_t>(ReadUint24BE(p + 16)) * 64ULL;
    report.feedback_packet_count = p[19];

    std::vector<uint8_t> statuses;
    statuses.reserve(report.packet_status_count);

    size_t off = 20;
    while (statuses.size() < report.packet_status_count && off + 2 <= len)
    {
        const uint16_t chunk = utils::Utils::ReadUint16BE(p + off);
        off += 2;

        if ((chunk & 0x8000) == 0)
        {
            const uint8_t symbol = static_cast<uint8_t>((chunk >> 13) & 0x03);
            const uint16_t run_length = static_cast<uint16_t>(chunk & 0x1FFF);
            for (uint16_t i = 0; i < run_length && statuses.size() < report.packet_status_count; ++i)
            {
                statuses.push_back(symbol);
            }
            continue;
        }

        const bool two_bit_symbols = (chunk & 0x4000) != 0;
        if (two_bit_symbols)
        {
            for (int shift = 12; shift >= 0 && statuses.size() < report.packet_status_count; shift -= 2)
            {
                statuses.push_back(static_cast<uint8_t>((chunk >> shift) & 0x03));
            }
        }
        else
        {
            for (int shift = 13; shift >= 0 && statuses.size() < report.packet_status_count; --shift)
            {
                statuses.push_back(static_cast<uint8_t>((chunk >> shift) & 0x01));
            }
        }
    }

    if (statuses.size() < report.packet_status_count)
    {
        return false;
    }

    int64_t receive_delta_us = 0;
    report.packets.reserve(statuses.size());

    for (size_t i = 0; i < statuses.size(); ++i)
    {
        rtcpx::TransportFeedbackPacket packet;
        packet.transport_sequence = static_cast<uint16_t>(report.base_sequence + static_cast<uint16_t>(i));

        const uint8_t status = statuses[i];
        if (status == 0)
        {
            packet.received = false;
            report.packets.push_back(packet);
            continue;
        }

        int32_t delta_us = 0;
        if (status == 1)
        {
            if (off + 1 > len)
            {
                return false;
            }
            delta_us = static_cast<int32_t>(p[off]) * 250;
            off += 1;
        }
        else if (status == 2)
        {
            if (off + 2 > len)
            {
                return false;
            }
            const int16_t delta = static_cast<int16_t>(utils::Utils::ReadUint16BE(p + off));
            delta_us = static_cast<int32_t>(delta) * 250;
            off += 2;
        }
        else
        {
            return false;
        }

        receive_delta_us += delta_us;
        packet.received = true;
        packet.receive_time_ms = report.reference_time_ms +
                                 static_cast<uint64_t>(receive_delta_us >= 0 ? receive_delta_us : 0) / 1000ULL;
        report.packets.push_back(packet);
    }

    return true;
}
void ExpandNackPair(uint16_t pid, uint16_t blp, std::vector<uint16_t>& seqs)
{
    seqs.push_back(pid);
    for (uint16_t i = 0; i < 16; ++i)
    {
        if (blp & (1u << i))
        {
            seqs.push_back(static_cast<uint16_t>(pid + i + 1));
        }
    }
}
}

rtcpx::RtcpReceiverImpl::RtcpReceiverImpl(rtcpx::IRtcpObserver* observer)
    : observer_(observer) {}

rtcpx::RtcpReceiverImpl::~RtcpReceiverImpl() = default;

bool rtcpx::RtcpReceiverImpl::OnRtcpPacket(const uint8_t* data, size_t len)
{
    if(!data || len < RtcpHeader::kHeaderSize)
    {
        return false;
    }

    size_t offset = 0;
    bool parsed_any = false;

    while (offset + RtcpHeader::kHeaderSize <= len)
    {
        RtcpHeader header;

        if (!header.Parse(data + offset, len - offset))
        {
            return false;
        }

        HandleSingleRtcpPacket(header.Data(), header.ContentSize(), header.CountOrFmt(), header.PacketType());
        offset += header.PacketSize();
        parsed_any = true;
    }
    return parsed_any && offset == len;
}

void rtcpx::RtcpReceiverImpl::SetObserver(IRtcpObserver* obs)
{
    observer_ = obs;
}
void rtcpx::RtcpReceiverImpl::SetLocalSsrc(uint32_t ssrc)
{
    local_ssrc_ = ssrc;
}
void rtcpx::RtcpReceiverImpl::SetRemoteSsrc(uint32_t ssrc)
{
    remote_ssrc_ = ssrc;
}

void rtcpx::RtcpReceiverImpl::HandleSingleRtcpPacket(const uint8_t* p, size_t len, uint8_t fmt, uint8_t pt)
{
    if (!observer_ || !p || len < 4)
    {
        return;
    }

    switch (pt)
    {
    case 200: // SR
        if (len >= 28)
        {
            const uint32_t sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
            const uint64_t ntp = utils::Utils::ReadUint64BE(p + 8);
            const uint32_t rtp_ts = utils::Utils::ReadUint32BE(p + 16);
            const uint32_t packet_count = utils::Utils::ReadUint32BE(p + 20);
            const uint32_t octet_count = utils::Utils::ReadUint32BE(p + 24);
            const uint32_t ntp_seconds = static_cast<uint32_t>(ntp >> 32);
            const uint32_t ntp_fraction = static_cast<uint32_t>(ntp);

            LOG_INFO("[RTCP][SR] sender_ssrc=", sender_ssrc,
                     " ntp_seconds=", ntp_seconds,
                     " ntp_fraction=", ntp_fraction,
                     " rtp_ts=", rtp_ts,
                     " packets=", packet_count,
                     " octets=", octet_count,
                     " report_blocks=", static_cast<uint32_t>(fmt));

            observer_->OnSenderReport(sender_ssrc, ntp, rtp_ts, packet_count, octet_count);

            size_t off = 28;
            for (uint8_t i = 0; i < fmt && off + 24 <= len; ++i, off += 24)
            {
                // Each report block is 24 bytes and starts with the media SSRC.
                const uint8_t* rb = p + off;
                observer_->OnReceiverReport(sender_ssrc,
                                            utils::Utils::ReadUint32BE(rb),
                                            rb[4],
                                            ReadInt24BE(rb + 5),
                                            utils::Utils::ReadUint32BE(rb + 8),
                                            utils::Utils::ReadUint32BE(rb + 12),
                                            utils::Utils::ReadUint32BE(rb + 16),
                                            utils::Utils::ReadUint32BE(rb + 20));
            }
        }
        break;
    case 201: // RR
        if (len >= 8)
        {
            const uint32_t sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
            LOG_INFO("[RTCP][RR] reporter_ssrc=", sender_ssrc,
                     " report_blocks=", static_cast<uint32_t>(fmt));

            size_t off = 8;
            for (uint8_t i = 0; i < fmt && off + 24 <= len; ++i, off += 24)
            {
                const uint8_t* rb = p + off;
                const uint32_t media_ssrc = utils::Utils::ReadUint32BE(rb);
                const uint8_t fraction_lost = rb[4];
                const int32_t cumulative_lost = ReadInt24BE(rb + 5);

                LOG_INFO("[RTCP][RR] reporter_ssrc=", sender_ssrc,
                         " media_ssrc=", media_ssrc,
                         " fraction_lost=", static_cast<uint32_t>(fraction_lost),
                         " cumulative_lost=", cumulative_lost);

                observer_->OnReceiverReport(sender_ssrc,
                                            media_ssrc,
                                            fraction_lost,
                                            cumulative_lost,
                                            utils::Utils::ReadUint32BE(rb + 8),
                                            utils::Utils::ReadUint32BE(rb + 12),
                                            utils::Utils::ReadUint32BE(rb + 16),
                                            utils::Utils::ReadUint32BE(rb + 20));
            }
        }
        break;
    case 202: // SDES
        // SDES is intentionally skipped for now. It is useful for CNAME/MID/RID
        // metadata, but it does not currently drive weak-network logic.
        
        break;
    case 203: // BYE
        if (len >= 4)
        {
            // BYE contains RC SSRC/CSRC identifiers, followed optionally by a
            // reason string. The current observer only needs the leaving SSRCs.
            size_t off = 4;
            for (uint8_t i = 0; i < fmt && off + 4 <= len; ++i, off += 4)
            {
                observer_->OnBye(utils::Utils::ReadUint32BE(p + off));
            }
        }
        break;
    case 205: // RTPFB
        if (len >= 12 && fmt == 1)
        {
            // RTPFB FMT=1 is Generic NACK. The feedback control information is
            // a list of 4-byte PID/BLP pairs after sender/media SSRC.
            const uint32_t sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
            const uint32_t media_ssrc = utils::Utils::ReadUint32BE(p + 8);
            std::vector<uint16_t> seqs;
            for (size_t off = 12; off + 4 <= len; off += 4)
            {
                const uint16_t pid = ReadUint16BE(p + off);
                const uint16_t blp = ReadUint16BE(p + off + 2);
                ExpandNackPair(pid, blp, seqs);
            }
            if (!seqs.empty())
            {
                observer_->OnNack(sender_ssrc, media_ssrc, seqs.data(), seqs.size());
            }
        }
        else if (len >= 20 && fmt == 15)
        {
            rtcpx::TransportFeedbackReport report;
            if (ParseTransportFeedback(p, len, report))
            {
                observer_->OnTransportFeedback(report);
            }
        }
        break;
    case 206: // PSFB
        if (len >= 12)
        {
            // Payload-specific feedback. FMT=1 is PLI; FMT=4 is FIR.
            const uint32_t sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
            const uint32_t media_ssrc = utils::Utils::ReadUint32BE(p + 8);
            if (fmt == 1)
            {
                observer_->OnPli(sender_ssrc, media_ssrc);
            }
            else if (fmt == 4)
            {
                // FIR entries are 8 bytes each. The packet-level media SSRC may
                // be zero, so prefer the entry SSRC when present.
                for (size_t off = 12; off + 8 <= len; off += 8)
                {
                    const uint32_t fir_media_ssrc = utils::Utils::ReadUint32BE(p + off);
                    const uint8_t seq_nr = p[off + 4];
                    observer_->OnFir(sender_ssrc, fir_media_ssrc ? fir_media_ssrc : media_ssrc, seq_nr);
                }
            }
        }
        break;
    default:
        break;
    }
}
