#include "SipConnection.h"

SipConnection::SipConnection(SipServer *server, TaskScheduler *scheduler, SOCKET fd)
   : TcpConnection(scheduler, sockfd)
   , sip_request_(std::make_unique<SipRequest>())
    ,sip_response_(std::make_unique<SipResponse>())
{
    
}