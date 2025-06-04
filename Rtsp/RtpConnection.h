#ifndef _RTPCONNECTION_H_
#define _RTPCONNECTION_H_

#include <memory>
#include "TcpConnection.h"


class RtspConnection;

class RtpConnection
{
public:
    RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection);


private:


};


#endif // _RTPCONNECTION_H_