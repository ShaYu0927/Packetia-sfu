#pragma once
#include "third/nlohmann/json.hpp"
#include <cstdint>
namespace server::cluster
{
struct HttpJsonReply { uint16_t status = 0; nlohmann::json body; };
// 只能在控制线程或 API worker 调用；有连接/总时限、响应大小上限，不跟随重定向。
HttpJsonReply PostControlJson(const std::string& url, const std::string& credential,
                             const nlohmann::json& body, long timeout_ms = 1500);
}
