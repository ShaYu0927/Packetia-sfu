#include "NodeClient.h"
#include "third/nlohmann/json.hpp"
#include "logger.h"
#include "ControlHttpClient.h"
#include <stdexcept>

namespace server::cluster
{
NodeClient::NodeClient(service::cluster::Node node, std::string control_url, std::string credential,
    std::shared_ptr<service::cluster::NodeLease> lease, std::shared_ptr<WebRtcService> rtc)
    : node_(std::move(node)), control_url_(std::move(control_url)), credential_(std::move(credential)),
      lease_(std::move(lease)), rtc_(std::move(rtc))
{
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
    Json body{{"node_id", node_.id}, {"instance_id", node_.instance}, {"participants", rtc_->ParticipantCount()}};
    if (registration) { body["signaling_url"] = node_.signaling_url; body["max_rooms"] = node_.max_rooms; }
    if (registration && !node_.management_url.empty()) body["management_url"] = node_.management_url;
    const auto url = control_url_ + (registration ? "/internal/v1/nodes/register" : "/internal/v1/nodes/heartbeat");
    const auto reply = PostControlJson(url, credential_, body);
    const auto status = reply.status;
    if (status == 200)
    {
        const auto& json = reply.body;
        if (!json.is_object() || !json.contains("lease_seconds") || !json["lease_seconds"].is_number_integer() ||
            json["lease_seconds"] != service::cluster::Coordinator::kLeaseSeconds) return 0;
        lease_->Renew(request_start, service::cluster::Coordinator::kLeaseSeconds);
        Reclaim(json, request_start);
    }
    else if (status == 401 || status == 409) lease_->Revoke();
    return status;
}
void NodeClient::Reclaim(const nlohmann::json& reply, int64_t started)
{
    using Json = nlohmann::json;
    const auto release = [&](const std::string& room, const std::string& assignment) {
        const auto ack = PostControlJson(control_url_ + "/internal/v1/rooms/release", credential_,
            {{"node_id", node_.id}, {"instance_id", node_.instance}, {"room_id", room}, {"assignment_id", assignment}});
        if (ack.status == 200) rtc_->ControlRoom(room, assignment, {{"type", "release_ack"}});
    };
    // 每轮限定工作时间；回收不能拖延下一次节点续租。
    for (const auto& room : rtc_->ReleasedRooms())
    {
        if (service::cluster::NodeLease::NowMs() - started > 2500) return;
        release(room.room_id, room.assignment_id);
    }
    const auto candidates = reply.find("reclaim");
    if (candidates == reply.end() || !candidates->is_array() || candidates->size() > 16) return;
    for (const auto& room : *candidates)
    {
        if (service::cluster::NodeLease::NowMs() - started > 2500) return;
        if (!room.is_object() || !room.contains("room_id") || !room["room_id"].is_string() ||
            !room.contains("assignment_id") || !room["assignment_id"].is_string() ||
            !room.contains("closing") || !room["closing"].is_boolean()) continue;
        const auto id = room["room_id"].get<std::string>(), assignment = room["assignment_id"].get<std::string>();
        const auto result = rtc_->ControlRoom(id, assignment, {{"type", room["closing"].get<bool>() ? "end_room" : "reclaim_room"}});
        if (result.value("released", false)) release(id, assignment);
    }
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
