#ifndef _SIGNAL_CODEC_H_
#define _SIGNAL_CODEC_H_

#include "SignalMessage.h"
#include <string>

namespace protocol
{
    
class SignalCodec
{
public:
    static bool Decode(const std::string& text, SignalMessage& msg);
    static std::string EncodeResponse(const std::string& requestId, int code, const std::string& message, const nlohmann::json& data = {});

    static std::string EncodeNotify(const std::string& event,const nlohmann::json& data);
};

}
#endif /* _SIGNAL_CODEC_H_ */