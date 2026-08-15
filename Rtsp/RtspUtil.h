#ifndef _RTSP_UTIL_H_
#define _RTSP_UTIL_H_

#include <string>
#include <optional>

#include "Rtsp.h"

namespace rtsp
{

class RtspUtil 
{
public:
   
    static std::optional<int> ParseStreamId(const std::string& control);
    static std::optional<int> ParseTrackId(const std::string& control);
    static std::string GetSuffixFromSetupUrl(const std::string& url);
    static bool ParseTransport(const std::string& text, RtspTransport& out);
    static std::string StripFmtpPayloadPrefix(const std::string& fmtp_line);
    static bool ParseRtpMapLine(const std::string& line, int* payload_type, std::string* codec_name, uint32_t* clock_rate, int* channels);
    static uint16_t Unwrap(uint16_t sequence_number);
};


class RtpSequenceNumberUnwrapper
{
public:
    int64_t Unwrap(uint16_t sequence_number);
    void Reset();

private:
    bool initialized_ = false;
    int64_t last_unwrapped_ = 0;
};

}


#endif
