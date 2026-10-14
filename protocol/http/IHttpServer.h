#ifndef PACKETIA_PROTOCOL_HTTP_I_SERVER_H_
#define PACKETIA_PROTOCOL_HTTP_I_SERVER_H_

#include "HttpServerOptions.h"
#include "HttpMessage.h"

#include <functional>

namespace protocol::http
{

class IHttpServer
{
public:
    using Respond = std::function<void(HttpResponse)>;
    using RequestHandler = std::function<void(HttpRequest, Respond)>;

    virtual ~IHttpServer() = default;

    virtual bool Start(const HttpServerOptions& options, RequestHandler handler) = 0;

    virtual void Stop() noexcept = 0;
};

}

#endif // PACKETIA_PROTOCOL_HTTP_I_SERVER_H_
