#pragma once

#include "ProtocolParser.h"

class SipParse : public ProtocolParser {
public:
    virtual ~SipParse() {}

    virtual bool Parse(BufferReader& buffer) override;
    virtual bool CanHandle(BufferReader& buffer) override;
};