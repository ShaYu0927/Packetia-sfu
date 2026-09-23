#ifndef _SDP_H_
#define _SDP_H_

#include "SdpMode.h"


namespace sdp {


enum class SdpErrorCode
{
    None = 0,
    InvalidSyntax,
    InvalidVersion,
    InvalidOrigin,
    InvalidSessionName,
    InvalidConnection,
    InvalidTiming,
    InvalidMedia,
    InvalidAttribute,
    UnsupportedField,
};

struct SdpParseResult
{
    bool ok = false;
    SdpSession session;
    SdpErrorCode code = SdpErrorCode::None;
    size_t line = 0;
    std::string message;
};

class Sdp
{
public:
    // Generic keeps raw attributes for RTSP. WebRtc additionally extracts and
    // validates ICE/DTLS/RTP attributes in the same session/media model.
    static SdpParseResult Parse(const std::string& text, SdpProfile profile = SdpProfile::Generic);
    // Transactional overload for signaling callers; failure preserves output.
    static bool Parse(const std::string& text, SdpProfile profile, SdpType type, SdpSession& output, std::string& error);
    static std::string Serialize(const SdpSession& session);
};

}


#endif /* _SDP_H_ */
