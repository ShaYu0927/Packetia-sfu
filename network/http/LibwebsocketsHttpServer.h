#ifndef PACKETIA_NETWORK_HTTP_LIBWEBSOCKETS_SERVER_H_
#define PACKETIA_NETWORK_HTTP_LIBWEBSOCKETS_SERVER_H_

#include "HttpServer.h"

namespace network::http
{

// Adapter extension point. No listener is opened until the actual HTTP parser,
// request lifecycle and asynchronous writable integration are implemented.
class LibwebsocketsHttpServer final : public HttpServer
{
public:
    bool Start(const HttpOptions& options, RequestHandler handler) override;
    void Stop() noexcept override;
};

}

#endif // PACKETIA_NETWORK_HTTP_LIBWEBSOCKETS_SERVER_H_
