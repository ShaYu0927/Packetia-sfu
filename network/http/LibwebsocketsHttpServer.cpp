#include "LibwebsocketsHttpServer.h"
#include "log/logger.h"

namespace network::http
{

bool LibwebsocketsHttpServer::Start(const HttpOptions&, RequestHandler)
{
    // TODO: reuse libwebsockets HTTP callbacks, enforce request limits while
    // receiving, and implement thread-safe owner-loop response delivery.
    LOG_ERROR("[HttpServer] HTTP listener is not implemented; refusing to start");
    return false;
}

void LibwebsocketsHttpServer::Stop() noexcept
{
}

}
