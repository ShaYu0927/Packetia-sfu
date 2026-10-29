#include "ControlHttpClient.h"
#include <curl/curl.h>
#include <memory>
#include <stdexcept>
namespace server::cluster
{
namespace
{
struct CurlRuntime
{
    CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl init failed"); }
    ~CurlRuntime() { curl_global_cleanup(); }
};
std::size_t Write(char* data, std::size_t size, std::size_t count, void* output)
{
    const auto bytes = size * count;
    auto& body = *static_cast<std::string*>(output);
    if (bytes > 512 * 1024 || body.size() > 512 * 1024 - bytes) return 0;
    try { body.append(data, bytes); return bytes; } catch (...) { return 0; }
}
}
HttpJsonReply PostControlJson(const std::string& url, const std::string& credential, const nlohmann::json& body, long timeout_ms)
{
    static CurlRuntime runtime;
    if (credential.empty() || credential.find_first_of("\r\n") != std::string::npos) return {};
    const auto encoded = body.dump();
    const auto authorization = "Authorization: Bearer " + credential;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return {};
    curl_slist* raw = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!raw) return {};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    if (!curl_slist_append(raw, authorization.c_str())) return {};
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, encoded.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(encoded.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    if (curl_easy_perform(curl.get()) != CURLE_OK) return {};
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    auto json = nlohmann::json::parse(response, nullptr, false);
    if (!json.is_object() || status < 100 || status > 599) return {};
    return {static_cast<uint16_t>(status), std::move(json)};
}
}
