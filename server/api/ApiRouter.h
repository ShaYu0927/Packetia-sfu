#ifndef PACKETIA_SERVER_API_ROUTER_H_
#define PACKETIA_SERVER_API_ROUTER_H_

#include "HttpMessage.h"
#include "nlohmann/json.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace server::api
{


struct ApiRequest
{
    const protocol::http::HttpRequest& http;
    const nlohmann::json& json; 
};

class ApiRouter final
{
public:
    enum class BodyMode { Raw, Json, Empty };
    using Handler = std::function<protocol::http::HttpResponse(const ApiRequest&)>;

private:
    struct Route
    {
        Handler handler;
        BodyMode body_mode;
    };
    using Methods = std::map<std::string, Route, std::less<>>;
    using Routes = std::map<std::string, Methods, std::less<>>;

public:
    class Builder
    {
    public:
        Builder& Add(std::string method, std::string path, Handler handler, BodyMode body_mode = BodyMode::Raw);
        Builder& Get(std::string path, Handler handler);
        Builder& Post(std::string path, Handler handler, BodyMode body_mode = BodyMode::Raw);
        Builder& PostJson(std::string path, Handler handler);
        Builder& Delete(std::string path, Handler handler);
        ApiRouter Build(std::size_t max_body_bytes = 8192) const;

    private:
        Routes routes_;
    };

    protocol::http::HttpResponse Dispatch(const protocol::http::HttpRequest& request) const;

private:
    ApiRouter(Routes routes, std::size_t max_body_bytes);

    const Routes routes_;
    const std::size_t max_body_bytes_;
};

}

#endif // PACKETIA_SERVER_API_ROUTER_H_
