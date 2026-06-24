#pragma once

#include "ProtocolParser.h"

namespace sip
{

class SipParser : public protocol::ProtocolParser
{
public:
    ParseResult Parse(BufferReader& buffer) override;
    const char* Name() const override { return "SIP"; }
};

} // namespace sip