#include "Sdp.h"
#include "SdpUtil.h"

namespace sdp 
{
SdpParseResult Sdp::Parse(const std::string& text)
{
    SdpParseResult result;
    std::string err;
    SdpSession tmp;

    if (!SdpParser::Parse(text, tmp, err))
    {
        result.ok = false;
        result.message = err;
        result.code = SdpErrorCode::InvalidSyntax;
        return result;
    }

    result.ok = true;
    result.session = std::move(tmp);
    return result;
}

std::string Sdp::Serialize(const SdpSession& session)
{
    return "";
}
}