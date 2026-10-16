#include "VideoRtpDepacketizerFactory.h"
#include "H264RtpDepacketizer.h"

#include <algorithm>
#include <cctype>

namespace media
{

std::unique_ptr<IVideoRtpDepacketizer> VideoRtpDepacketizerFactory::Create(
    CodecType codec, const std::string& encoding_name,
    uint32_t clock_rate, const std::string& fmtp)
{
    auto encoding = encoding_name;
    std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    switch (codec)
    {
    case CodecType::H264:
        if ((!encoding.empty() && encoding != "H264") ||
            (clock_rate != 0 && clock_rate != 90000))
            return nullptr;
        return std::make_unique<H264RtpDepacketizer>(fmtp);
    default:
        return nullptr;
    }
}

} // namespace media
