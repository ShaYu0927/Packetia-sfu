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
    static SdpParseResult Parse(const std::string& text);
    static std::string Serialize(const SdpSession& session);
};

}


#endif /* _SDP_H_ */