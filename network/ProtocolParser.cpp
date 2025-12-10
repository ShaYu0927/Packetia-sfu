#include "ProtocolParser.h"

ProtocolParser::Ptr ProtocolDetector::Detect(BufferReader &buffer)
{
    size_t size = buffer.ReadableBytes();
    for (const auto& rule : rules_)
    {
        if (size >= rule.min_bytes) 
        {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.Peek());
            if (rule.matcher(data, size)) 
            {
                return rule.creator();
            }
        }
    }
    return nullptr;
}
