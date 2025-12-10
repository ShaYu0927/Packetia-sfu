#include "SipParse.h"

ParseResult SipParse::Parse(BufferReader &buffer)
{
    LOG_INFO("SIP Parser invoked");
    return ParseResult::NeedMoreData;
}
