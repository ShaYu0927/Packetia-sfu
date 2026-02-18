#ifndef _PROTOCOLPARSER_H_
#define _PROTOCOLPARSER_H_

#include "BufferRead.h"
#include <functional>
#include "TcpSession.h"

namespace protocol
{
enum class DetectResult 
{
    Undecided,  
    Matched,   
};

class ProtocolParser 
{
public:
    enum class ParseResult 
    {
        Ok,
        NeedMoreData,
        Error
    };
    using Ptr = std::shared_ptr<ProtocolParser>;
    virtual ~ProtocolParser() = default;

    virtual ParseResult Parse(BufferReader& buffer) = 0;
    virtual const char* Name() const = 0;
};


}

#endif // _PROTOCOLPARSER_H_