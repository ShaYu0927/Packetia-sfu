#pragma once

#include "BufferRead.h"

class ProtocolParser {
public:
    virtual ~ProtocolParser() {}
    
    virtual bool Parse(BufferReader& buffer) = 0; // 解析数据
    virtual bool CanHandle(BufferReader& buffer) = 0; // 协议探测
};
