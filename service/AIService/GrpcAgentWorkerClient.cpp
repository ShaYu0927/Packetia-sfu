#include "GrpcAgentWorkerClient.h"
#include "agent/AgentRpcConfig.h"
#include "agent_worker.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace service::ai
{
namespace
{
namespace rpc = packetia::agent::v1;
using Kind = AgentWorkerEvent::Kind;

class GrpcAgentWorkerClient final : public AgentWorker
{
    class Session final : public grpc::ClientBidiReactor<rpc::SessionJob, rpc::SessionJoined>
    {
    public:
        Session(GrpcAgentWorkerClient& owner, AgentWorkerJob job, EventHandler handler)
            : owner_(owner), handler_(std::move(handler))
        {
            request_.set_session_id(std::move(job.session_id));
            request_.set_room_id(std::move(job.room_id));
            request_.set_agent_id(std::move(job.agent_id));
            request_.set_signaling_url(std::move(job.signaling_url));
            request_.set_join_token(std::move(job.join_token));
        }

        void Run()
        {
            context_.AddMetadata(rpc_config::kTokenHeader, owner_.token_);
            owner_.stub_->async()->RunSession(&context_, this);
            AddHold();
            StartRead(&joined_);
            StartWrite(&request_);
            StartCall();
        }

        void RequestStop()
        {
            stop_requested_ = true;
            if (job_sent_) CloseWrites();
        }

        void Cancel() { context_.TryCancel(); }

    private:
        void OnWriteDone(bool ok) override
        {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            request_.clear_join_token();
            job_sent_ = ok;
            if (!ok) ReleaseWriteHold();
            else if (stop_requested_) CloseWrites();
        }

        void OnReadDone(bool ok) override
        {
            if (!ok)
            {
                std::lock_guard<std::mutex> lock(owner_.mutex_);
                ReleaseWriteHold();
                return;
            }
            handler_({Kind::Joined, std::move(*joined_.mutable_participant_id()), {}});
            StartRead(&joined_);
        }

        void OnDone(const grpc::Status& status) override
        {
            if (status.ok()) handler_({Kind::Stopped, {}, {}});
            else
            {
                std::string error = "agent_worker_unavailable";
                switch (status.error_code())
                {
                case grpc::StatusCode::ABORTED: error = status.error_message(); break;
                case grpc::StatusCode::RESOURCE_EXHAUSTED: error = "agent_worker_capacity_reached"; break;
                case grpc::StatusCode::UNAUTHENTICATED: error = "agent_worker_unauthorized"; break;
                case grpc::StatusCode::ALREADY_EXISTS: error = "agent_id_conflict"; break;
                default: break;
                }
                handler_({Kind::Failed, {}, std::move(error)});
            }
            const auto id = request_.session_id();
            owner_.RemoveSession(id);
        }

        void CloseWrites()
        {
            if (!write_held_) return;
            StartWritesDone();
            ReleaseWriteHold();
        }

        void ReleaseWriteHold()
        {
            if (std::exchange(write_held_, false)) RemoveHold();
        }

        GrpcAgentWorkerClient& owner_;
        EventHandler handler_;
        grpc::ClientContext context_;
        rpc::SessionJob request_;
        rpc::SessionJoined joined_;
        bool write_held_ = true;
        bool job_sent_ = false;
        bool stop_requested_ = false;
    };

public:
    GrpcAgentWorkerClient(const std::string& address, std::string token)
        : token_(std::move(token)), stub_(rpc::AgentWorker::NewStub(
            grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))) {}

    ~GrpcAgentWorkerClient() override { Stop(); }

    bool Start() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !std::exchange(accepting_, true);
    }

    void Stop() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        accepting_ = false;
        for (auto& entry : sessions_) entry.second->RequestStop();
        const auto empty = [this] { return sessions_.empty(); };
        if (!drained_.wait_for(lock, std::chrono::seconds(3), empty))
        {
            for (auto& entry : sessions_) entry.second->Cancel();
            drained_.wait(lock, empty);
        }
    }

    bool StartSession(AgentWorkerJob job, EventHandler handler) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || sessions_.count(job.session_id)) return false;
        const auto id = job.session_id;
        auto session = std::make_unique<Session>(*this, std::move(job), std::move(handler));
        sessions_.emplace(id, std::move(session)).first->second->Run();
        return true;
    }

    void StopSession(const std::string& id) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) found->second->RequestStop();
    }

private:
    void RemoveSession(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(id);
        if (sessions_.empty()) drained_.notify_all();
    }

    const std::string token_;
    const std::unique_ptr<rpc::AgentWorker::Stub> stub_;
    std::mutex mutex_;
    std::condition_variable drained_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    bool accepting_ = false;
};
}

std::shared_ptr<AgentWorker> MakeGrpcAgentWorkerClient(std::string address)
{
    rpc_config::RequireLoopbackAddress(address);
    return std::make_shared<GrpcAgentWorkerClient>(address, rpc_config::Token());
}
}
