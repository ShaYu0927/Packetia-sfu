#pragma once

#include "ProtocolParser.h"
#include "logger.h"

class SipParse : public ProtocolParser 
{
public:
    virtual ~SipParse() {}

    virtual ParseResult Parse(BufferReader& buffer) override;
    virtual const char* Name() const override { return "SIP"; }
};