#ifndef _SDP_UTIL_H_
#define _SDP_UTIL_H_

#include "SdpMode.h"

namespace sdp 
{

class SdpParser
{
public:
    static bool Parse(const std::string& text, SdpSession& session, std::string& err);

private:
    static bool ParseLines(const std::vector<SdpLine>& lines, SdpSession& session, std::string& err);

    static bool ParseVersion(const SdpLine& line, SdpSession& session, std::string& err);
    static bool ParseOrigin(const SdpLine& line, SdpSession& session, std::string& err);
    static bool ParseSessionName(const SdpLine& line, SdpSession& session, std::string& err);
    static bool ParseConnection(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err);
    static bool ParseTiming(const SdpLine& line, SdpSession& session, std::string& err);
    static bool ParseMedia(const SdpLine& line, SdpSession& session, SdpMedia*& current_media, std::string& err);
    static bool ParseAttribute(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err);

    static SdpAttribute SplitAttribute(const std::string& text);
};

} // namespace sdp


#endif /* _SDP_UTIL_H_ */