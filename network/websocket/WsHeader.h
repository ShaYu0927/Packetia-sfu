#ifndef _WS_HEADER_H_
#define _WS_HEADER_H_

#include "hv/hsocket.h"
#include "hv/hloop.h"
#include "hv/Buffer.h"
#include "hv/Channel.h"

#include "server/WebSocketServer.h"

#ifndef DEFAULT_CONNECT_TIMEOUT
#define DEFAULT_CONNECT_TIMEOUT DEFAULT_HTTP_CONNECT_TIMEOUT
#endif

#endif /* _WS_HEADER_H_ */