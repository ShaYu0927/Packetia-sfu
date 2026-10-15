#include "NodeClient.h"
#include "third/nlohmann/json.hpp"
#include "log/logger.h"
#include <curl/curl.h>
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
    if (bytes > 8192 || body.size() > 8192 - bytes) return 0;
    try { body.append(data, bytes); return bytes; } catch (...) { return 0; }
}
}
NodeClient::NodeClient(service::cluster::Node node, std::string control_url, std::string credential,
    std::shared_ptr<service::cluster::NodeLease> lease, std::function<std::size_t()> load)
    : node_(std::move(node)), control_url_(std::move(control_url)), credential_(std::move(credential)),
      lease_(std::move(lease)), load_(std::move(load))
{
    static CurlRuntime runtime;
    if ((control_url_.rfind("http://", 0) != 0 && control_url_.rfind("https://", 0) != 0) ||
        control_url_.find_first_of("?#@\r\n ") != std::string::npos || control_url_.back() == '/' ||
        credential_.size() < 32 || credential_.size() > 4096 || credential_.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("invalid control URL or node credential");
}
NodeClient::~NodeClient() { Stop(); }
long NodeClient::Send(bool registration)
{
    using Json = nlohmann::json;
    const auto request_start = service::cluster::NodeLease::NowMs();
    Json body{{"node_id", node_.id}, {"instance_id", node_.instance}, {"participants", load_()}};
    if (registration) { body["signaling_url"] = node_.signaling_url; body["max_rooms"] = node_.max_rooms; }
    const auto encoded = body.dump();
    const auto url = control_url_ + (registration ? "/internal/v1/nodes/register" : "/internal/v1/nodes/heartbeat");
    const auto authorization = "Authorization: Bearer " + credential_;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return 0;
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    if (!raw_headers) return 0;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    const auto appended = curl_slist_append(raw_headers, authorization.c_str());
    if (!appended) return 0;
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, encoded.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(encoded.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) return 0;
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status == 200)
    {
        const auto json = Json::parse(response, nullptr, false);
        if (!json.is_object() || !json.contains("lease_seconds") || !json["lease_seconds"].is_number_integer() ||
            json["lease_seconds"] != service::cluster::Coordinator::kLeaseSeconds) return 0;
        lease_->Renew(request_start, service::cluster::Coordinator::kLeaseSeconds);
    }
    else if (status == 401 || status == 409) lease_->Revoke();
    return status;
}
bool NodeClient::Start()
{
    if (thread_.Joinable()) return true;
    // A previous process's lease can outlive a graceful stop or container restart.
    // Wait for it to expire, while the admission guard remains closed.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    long status = 0;
    while ((status = Send(true)) != 200)
    {
        if ((status != 0 && status != 409 && status != 503) || std::chrono::steady_clock::now() >= deadline)
        {
            LOG_ERROR("Node registration failed, HTTP status=", status);
            return false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping_; })) return false;
    }
    stopping_ = false;
    thread_ = utils::ThreadManager::Instance().Start({"cluster", "node-heartbeat", utils::ThreadModel::Dedicated},
        [this] { Run(); }, [this] {
            { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
            lease_->Revoke(); wake_.notify_all();
        });
    return true;
}
void NodeClient::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; }))
    {
        lock.unlock();
        long status = 0;
        try { status = Send(false); } catch (...) { /* Lease expires on transient failures. */ }
        lock.lock();
        if (status == 401 || status == 409) { stopping_ = true; lease_->Revoke(); }
        if (stopping_) { lease_->Revoke(); break; }
    }
}
void NodeClient::Stop() noexcept
{
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    thread_.Join();
    lease_->Revoke();
}
}
