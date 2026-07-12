#include "RtcpNack.h"
#include <algorithm>
#include "Rtcp.h"
#include "utils.h"

namespace rtcpx 
{
std::vector<uint8_t> RtRtcpNack::Build(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t>& lost_seqs)
{
    std::vector<NackPair> pairs = PackPairs(lost_seqs);

    if(pairs.empty())
    {
        return {};
    }

    size_t packet_size = 12 + pairs.size() * 4;

    uint16_t length_words_minus_one = static_cast<uint16_t>((packet_size / 4) - 1);
    std::vector<uint8_t> packet(packet_size, 0);

    /*
     * Byte 0:
     *   V   = 2，占高 2 bit
     *   P   = 0
     *   FMT = 1，Generic NACK
     */

    packet[0] = static_cast<uint8_t>((RTCP_VERSION << 6) | RTCP_FMT_NACK);
    packet[1] = RTCP_PT_RTPFB;

    utils::Utils::WriteUint16BE(packet.data() + 2, length_words_minus_one);
    utils::Utils::WriteUint32BE(packet.data() + 4, sender_ssrc);
    utils::Utils::WriteUint32BE(packet.data() + 8, media_ssrc);

    size_t offset = 12;
    for (const auto& pair : pairs) 
    {
        utils::Utils::WriteUint16BE(packet.data() + offset, pair.pid);
        utils::Utils::WriteUint16BE(packet.data() + offset + 2, pair.blp);
        offset += 4;
    }

    return packet;
}


std::vector<RtRtcpNack::NackPair> RtRtcpNack::PackPairs(const std::vector<uint16_t>& lost_seqs)
{
    std::vector<RtRtcpNack::NackPair> pairs;
    if(lost_seqs.empty())
    {
        return pairs;
    }

    std::vector<uint16_t> seqs = lost_seqs;
    std::sort(seqs.begin(), seqs.end());
    seqs.erase(std::unique(seqs.begin(), seqs.end()), seqs.end());

    size_t i = 0;
    while(i < seqs.size())
    {
        uint16_t pid = seqs[i];
        uint16_t blp = 0;

        size_t j = i + 1;
        while (j < seqs.size())
        {
            uint16_t diff = static_cast<uint16_t>(seqs[j] - pid);
            /*
             * BLP 只能表示 PID 后面的 16 个包：
             *
             * bit0 -> pid + 1
             * bit1 -> pid + 2
             * ...
             * bit15 -> pid + 16
             */

             if(diff == 0)
             {
                ++j;
                continue;
             }


            if(diff > 16)
            {
                break;
            }

            blp |= static_cast<uint16_t>(1u << (diff - 1));
            ++j;
        }

        pairs.push_back(NackPair{pid, blp});
        ++i;
        while (i < seqs.size()) 
        {
            uint16_t diff = static_cast<uint16_t>(seqs[i] - pid);
            if (diff == 0 || diff > 16) 
            {
                break;
            }
            ++i;
        }
    }

    return pairs;
}


std::vector<uint16_t> RtRtcpNack::ExpandPairs(const std::vector<RtRtcpNack::NackPair>& pairs)
{
    std::vector<uint16_t> seqs;

    for (const auto& pair : pairs) 
    {
        seqs.push_back(pair.pid);

        for (uint16_t i = 0; i < 16; ++i) 
        {
            if (pair.blp & static_cast<uint16_t>(1u << i)) 
            {
                seqs.push_back(static_cast<uint16_t>(pair.pid + i + 1));
            }
        }
    }

    return seqs;
}


bool RtRtcpNack::Parse(const uint8_t* data, size_t len, uint32_t* sender_ssrc, uint32_t* media_ssrc, std::vector<uint16_t>* lost_seqs)
{
    if (data == nullptr || len < 12) 
    {
        return false;
    }

    uint8_t version = static_cast<uint8_t>(data[0] >> 6);
    uint8_t fmt = static_cast<uint8_t>(data[0] & 0x1F);
    uint8_t pt = data[1];

    if(version != RTCP_VERSION)
    {
        return false;
    }

    if (fmt != RTCP_FMT_NACK) 
    {
        return false;
    }

    if (pt != RTCP_PT_RTPFB) 
    {
        return false;
    }

    uint16_t length_words_minus_one = utils::Utils::ReadUint16BE(data + 2);
    size_t packet_size = (static_cast<size_t>(length_words_minus_one) + 1) * 4;

    if (packet_size < 12 || packet_size > len) 
    {
        return false;
    }

    if ((packet_size - 12) % 4 != 0) 
    {
        return false;
    }

    if (sender_ssrc) 
    {
        *sender_ssrc = utils::Utils::ReadUint32BE(data + 4);
    }

    if (media_ssrc) 
    {
        *media_ssrc = utils::Utils::ReadUint32BE(data + 8);
    }

    if(lost_seqs)
    {
        lost_seqs->clear();
        std::vector<NackPair> pairs;

        for (size_t offset = 12; offset + 4 <= packet_size; offset += 4) 
        {
            NackPair pair;
            pair.pid = utils::Utils::ReadUint16BE(data + offset);
            pair.blp = utils::Utils::ReadUint16BE(data + offset + 2);
            pairs.push_back(pair);
        }
        *lost_seqs = ExpandPairs(pairs);
    }
    return true;
}
}