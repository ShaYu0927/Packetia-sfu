#ifndef PACKETIA_PROTOCOL_HTTP_SERVER_H_
#define PACKETIA_PROTOCOL_HTTP_SERVER_H_

#include "IHttpServer.h"
#include <memory>

class EventLoop;
class TaskScheduler;

namespace protocol::http
{


class HttpServer final : public IHttpServer
{
public:
    explicit HttpServer(EventLoop* loop = nullptr);
    explicit HttpServer(std::shared_ptr<TaskScheduler> scheduler);
    ~HttpServer() override;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool Start(const HttpServerOptions& options, RequestHandler handler) override;
    void Stop() noexcept override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}

#endif // PACKETIA_PROTOCOL_HTTP_SERVER_H_
