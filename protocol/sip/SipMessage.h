#ifndef PACKETIA_PROTOCOL_SIP_SIPMESSAGE_H_
#define PACKETIA_PROTOCOL_SIP_SIPMESSAGE_H_

#include <memory>

class SipRequest : public std::enable_shared_from_this<SipRequest>
{
public:
    using Ptr = std::shared_ptr<SipRequest>;
};

class SipResponse : public std::enable_shared_from_this<SipResponse>
{
public: 
    using Ptr = std::shared_ptr<SipResponse>;
};

#endif // PACKETIA_PROTOCOL_SIP_SIPMESSAGE_H_
