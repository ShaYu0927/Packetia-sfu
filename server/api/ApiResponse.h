#ifndef PACKETIA_SERVER_API_RESPONSE_H_
#define PACKETIA_SERVER_API_RESPONSE_H_

#include "HttpMessage.h"
#include "third/nlohmann/json.hpp"

namespace server::api
{

protocol::http::HttpResponse JsonResponse(std::uint16_t status, nlohmann::json body);
protocol::http::HttpResponse ErrorResponse(std::uint16_t status, const char* code, const char* message);

}

#endif // PACKETIA_SERVER_API_RESPONSE_H_
