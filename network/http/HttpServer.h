#ifndef PACKETIA_NETWORK_HTTP_SERVER_H_
#define PACKETIA_NETWORK_HTTP_SERVER_H_

#include "HttpOptions.h"
#include "HttpTypes.h"

#include <functional>

namespace network::http
{

class HttpServer
{
public:
    using Respond = std::function<void(HttpResponse)>;
    using RequestHandler = std::function<void(HttpRequest, Respond)>;

    virtual ~HttpServer() = default;

    // Start/Stop are serialized by the owner. The transport owns HTTP parsing,
    // body/header limits, deadlines and connection lifetime. A handler receives
    // an owned request, never a borrowed parser buffer or raw socket.
    //
    // Respond may be called from a business worker. The adapter MUST marshal it
    // to the connection's I/O owner and discard it if that connection or request
    // generation has closed. It must not write a socket on the worker thread.
    // The handler calls Respond at most once; it must not throw through it.
    virtual bool Start(const HttpOptions& options, RequestHandler handler) = 0;

    // Stop accepting requests and invalidate outstanding response callbacks.
    // Repeated Stop is safe, including after an unsuccessful Start.
    virtual void Stop() noexcept = 0;
};

}

#endif // PACKETIA_NETWORK_HTTP_SERVER_H_
