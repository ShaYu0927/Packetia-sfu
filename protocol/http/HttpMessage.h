#ifndef PACKETIA_PROTOCOL_HTTP_MESSAGE_H_
#define PACKETIA_PROTOCOL_HTTP_MESSAGE_H_

#include <cstdint>
#include <map>
#include <string>

namespace protocol::http
{

using Headers = std::map<std::string, std::string, std::less<>>;

struct HttpRequest
{
    // The adapter supplies an uppercase method and URL path without the query.
    std::string method;
    std::string path;
    std::string body;
    // The adapter normalizes names to lowercase and rejects ambiguous duplicate
    // Authorization/Content-Length headers before constructing this request.
    Headers headers;
};

struct HttpResponse
{
    std::uint16_t status = 200;
    std::string body;
    Headers headers;
};

}

#endif // PACKETIA_PROTOCOL_HTTP_MESSAGE_H_
