#include "RtcpHealper.h"
#include "utils.h"

namespace rtcpx
{
bool RtcpHeader::Parse(const uint8_t* data, size_t len)
{
    Reset();

    if (data == nullptr || len < kRtcpCommonHeaderSize)
    {
        return false;
    }

    data_ = data;
    buffer_size_ = len;

    /*
     * RTCP common header:
     *
     * byte 0:
     *   V: 2 bits
     *   P: 1 bit
     *   RC/FMT: 5 bits
     *
     * byte 1:
     *   PT: 8 bits
     *
     * byte 2-3:
     *   length: 16 bits, network byte order
     */
    version_ = static_cast<uint8_t>(data[0] >> 6);
    padding_ = (data[0] & 0x20) != 0;
    count_or_fmt_ = static_cast<uint8_t>(data[0] & 0x1F);
    packet_type_ = data[1];

    words_minus_one_ = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | static_cast<uint16_t>(data[3]));

    if (version_ != 2)
    {
        return false;
    }

    /*
     * RTCP length 的单位是 32-bit word - 1。
     *
     * 所以真实包长：
     *   packet_size = (length + 1) * 4
     */
    packet_size_ = (static_cast<std::size_t>(words_minus_one_) + 1) * 4;

    if (packet_size_ < kRtcpCommonHeaderSize)
    {
        return false;
    }

    if (packet_size_ > len)
    {
        return false;
    }

    content_size_ = packet_size_;

    /*
     * 如果有 padding，最后一个字节表示 padding 总长度。
     * padding 长度本身也包含最后这个 count 字节。
     */
    if (padding_)
    {
        uint8_t padding_size = data[packet_size_ - 1];

        if (padding_size == 0)
        {
            return false;
        }

        if (padding_size > packet_size_)
        {
            return false;
        }

        content_size_ = packet_size_ - padding_size;

        if (content_size_ < kRtcpCommonHeaderSize)
        {
            return false;
        }
    }

    valid_ = true;
    return true;
}
}