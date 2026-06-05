#include "RtcpReciver.h"
#include "BufferRead.h"
#include "logger.h"
#include "utils.h"
#include "RtcpHealper.h"
#include <vector>

namespace
{
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

bool rtcpx::InspectRtcpPacket(const uint8_t* data, size_t len, RtcpPacketInfo* out)
{
    if (out)
    {
        *out = RtcpPacketInfo{};
    }

    if (!data || len < 4)
    {
        return false;
    }

    size_t offset = 0;
    bool parsed_any = false;
    RtcpPacketInfo info{};

    while (offset + 4 <= len)
    {
        const uint8_t* p = data + offset;
        const uint8_t version = (p[0] >> 6) & 0x03;
        if (version != 2)
        {
            return false;
        }

        const uint8_t count_or_fmt = p[0] & 0x1F;
        const uint8_t pt = p[1];
        const uint16_t words_minus1 = utils::Utils::ReadUint16BE(p + 2);
        const size_t pkt_len = (static_cast<size_t>(words_minus1) + 1) * 4;


        if (pkt_len < 4 || offset + pkt_len > len)
        {
            return false;
        }

        if (!parsed_any)
        {
            info.first_packet_type = pt;
            info.first_count_or_fmt = count_or_fmt;
        }

        if ((pt == 200 || pt == 201 || pt == 205 || pt == 206) && pkt_len >= 8)
        {
            info.sender_ssrc = utils::Utils::ReadUint32BE(p + 4);
            info.has_sender_ssrc = true;
        }

        if ((pt == 205 || pt == 206) && pkt_len >= 12)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(p + 8);
            info.has_media_ssrc = true;
        }
       
        else if (pt == 201 && count_or_fmt > 0 && pkt_len >= 32)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(p + 8);
            info.has_media_ssrc = true;
        }
        
        else if (pt == 200 && count_or_fmt > 0 && pkt_len >= 52)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(p + 28);
            info.has_media_ssrc = true;
        }
        
        else if ((pt == 200 || pt == 201) && !info.has_media_ssrc && pkt_len >= 8)
        {
            info.media_ssrc = utils::Utils::ReadUint32BE(p + 4);
            info.has_media_ssrc = true;
        }

        offset += pkt_len;
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
            observer_->OnSenderReport(sender_ssrc,
                                      WriteUint32LE(p + 8),
                                      utils::Utils::ReadUint32BE(p + 16),
                                      utils::Utils::ReadUint32BE(p + 20),
                                      utils::Utils::ReadUint32BE(p + 24));

            size_t off = 28;
            for (uint8_t i = 0; i < fmt && off + 24 <= len; ++i, off += 24)
            {
                // Each report block is 24 bytes and starts with the media SSRC.
                const uint8_t* rb = p + off;
                observer_->OnReceiverReport(sender_ssrc,
                                            utils::Utils::ReadUint32BE(rb),
                                            rb[4],
                                            utils::Utils::ReadUint32BE(rb + 5),
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
            size_t off = 8;
            for (uint8_t i = 0; i < fmt && off + 24 <= len; ++i, off += 24)
            {
                const uint8_t* rb = p + off;
                observer_->OnReceiverReport(sender_ssrc,
                                            utils::Utils::ReadUint32BE(rb),
                                            rb[4],
                                            utils::Utils::ReadUint32BE(rb + 5),
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
