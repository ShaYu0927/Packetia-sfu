#ifndef _RTCP_HEADER_H_
#define _RTCP_HEADER_H_

#include <cstdint>
#include <cstddef>
#include "Rtcp.h"

namespace rtcpx
{

enum RtcpPacketType : uint8_t
{
    RTCP_PT_SR    = 200, // Sender Report
    RTCP_PT_RR    = 201, // Receiver Report
    RTCP_PT_SDES  = 202, // Source Description
    RTCP_PT_BYE   = 203, // Goodbye
    RTCP_PT_APP   = 204, // Application-defined
    RTCP_PT_RTPFB = 205, // RTP Feedback, e.g. NACK/TWCC
    RTCP_PT_PSFB  = 206, // Payload Specific Feedback, e.g. PLI/FIR/REMB
    RTCP_PT_XR    = 207  // Extended Report
};

class RtcpHeader
{
public:
    static const std::size_t kRtcpCommonHeaderSize = 4;
    static const int kHeaderSize = 4;


public:
    RtcpHeader() = default;

    RtcpHeader(const uint8_t* data, std::size_t len)
    {
        Parse(data, len);
    }

    bool Parse(const uint8_t* data, std::size_t len);

    bool IsValid() const { return valid_; }

   
    bool IsVailid() const { return IsValid(); }

    const uint8_t* Data() const
    {
        return data_;
    }

    const uint8_t* Payload() const
    {
        return valid_ ? data_ + kRtcpCommonHeaderSize : nullptr;
    }

    std::size_t BufferSize() const
    {
        return buffer_size_;
    }

    std::size_t PacketSize() const
    {
        return packet_size_;
    }

    std::size_t ContentSize() const
    {
        return content_size_;
    }


    std::size_t PayloadSize() const
    {
        return content_size_ > kRtcpCommonHeaderSize ? content_size_ - kRtcpCommonHeaderSize : 0;
    }

    uint8_t Version() const
    {
        return version_;
    }

    bool HasPadding() const
    {
        return padding_;
    }


    uint8_t CountOrFmt() const
    {
        return count_or_fmt_;
    }

    uint8_t ReportCount() const
    {
        return count_or_fmt_;
    }

    uint8_t FeedbackMessageType() const
    {
        return count_or_fmt_;
    }

    uint8_t PacketType() const
    {
        return packet_type_;
    }

    uint16_t WordsMinusOne() const
    {
        return words_minus_one_;
    }

    bool IsSenderReport() const
    {
        return packet_type_ == RTCP_SR;
    }

    bool IsReceiverReport() const
    {
        return packet_type_ == RTCP_RR;
    }

    bool IsSdes() const
    {
        return packet_type_ == RTCP_SDES;
    }

    bool IsBye() const
    {
        return packet_type_ == RTCP_BYE;
    }

    bool IsApp() const
    {
        return packet_type_ == RTCP_APP;
    }

    bool IsRtpFeedback() const
    {
        return packet_type_ == RTCP_RTPFB;
    }

    bool IsPayloadFeedback() const
    {
        return packet_type_ == RTCP_PSFB;
    }

    void Reset()
    {
        data_ = nullptr;
        buffer_size_ = 0;

        valid_ = false;
        version_ = 0;
        padding_ = false;
        count_or_fmt_ = 0;
        packet_type_ = 0;
        words_minus_one_ = 0;

        packet_size_ = 0;
        content_size_ = 0;
    }

private:
    const uint8_t* data_ = nullptr;
    std::size_t buffer_size_ = 0;

    bool valid_ = false;

    uint8_t version_ = 0;
    bool padding_ = false;
    uint8_t count_or_fmt_ = 0;
    uint8_t packet_type_ = 0;
    uint16_t words_minus_one_ = 0;

    /*
     * packet_size_ 包含 RTCP header 和 padding。
     */
    std::size_t packet_size_ = 0;

    /*
     * content_size_ 不包含 padding，但包含 RTCP header。
     */
    std::size_t content_size_ = 0;
};


}


#endif /* _RTCP_HEADER_H_ */