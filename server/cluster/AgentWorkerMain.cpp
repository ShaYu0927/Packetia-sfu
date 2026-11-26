#include "LocalAgentWorker.h"
#include "WebSocketAgentRoomClient.h"
#include "agent/AgentRpcConfig.h"
#include "agent_worker.grpc.pb.h"
#include "logger.h"
#include <grpcpp/grpcpp.h>
#include <openssl/crypto.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace
{
namespace ai = service::ai;
namespace rpc = packetia::agent::v1;
using Kind = ai::AgentWorkerEvent::Kind;

constexpr std::size_t kMaxSessions = 128;
volatile std::sig_atomic_t stopping = 0;
void StopSignal(int) { stopping = 1; }

class WorkerService final : public rpc::AgentWorker::CallbackService
{
    class Session final : public grpc::ServerBidiReactor<rpc::SessionJob, rpc::SessionJoined>,
                          public std::enable_shared_from_this<Session>
    {
    public:
        explicit Session(WorkerService& service) : service_(service) {}

        static Session* Start(WorkerService& service, bool authorized)
        {
            auto session = std::make_shared<Session>(service);
            session->rpc_lifetime_ = session;
            if (authorized) session->StartRead(&session->request_);
            else session->Complete({grpc::StatusCode::UNAUTHENTICATED, "Agent RPC token rejected"});
            return session.get();
        }

    private:
        void OnReadDone(bool ok) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_) return;
            if (!id_.empty())
            {
                unexpected_request_ = ok;
                StopRoom();
                return;
            }
            if (!ok || request_.session_id().empty() || request_.room_id().empty() ||
                request_.agent_id().empty() || request_.signaling_url().empty() || request_.join_token().empty())
            {
                Complete({grpc::StatusCode::INVALID_ARGUMENT, "Agent job is missing required fields"});
                return;
            }

            const auto status = service_.Claim(request_.session_id());
            if (!status.ok())
            {
                Complete(status);
                return;
            }
            id_ = std::move(*request_.mutable_session_id());
            ai::AgentWorkerJob job{id_, std::move(*request_.mutable_room_id()),
                std::move(*request_.mutable_agent_id()), std::move(*request_.mutable_signaling_url()),
                std::move(*request_.mutable_join_token())};
            if (!service_.worker_.StartSession(std::move(job), [weak = weak_from_this()](ai::AgentWorkerEvent event) {
                if (const auto session = weak.lock()) session->OnWorkerEvent(std::move(event));
            }))
            {
                Complete({grpc::StatusCode::UNAVAILABLE, "Worker is stopped"});
                return;
            }
            StartRead(&request_);
        }

        void OnWriteDone(bool ok) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
            if (status_) Finish(*status_);
            else if (!ok) StopRoom();
        }

        void OnCancel() override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_) return;
            if (id_.empty()) Complete({grpc::StatusCode::CANCELLED, "Agent RPC cancelled"});
            else StopRoom();
        }

        void OnDone() override
        {
            if (!id_.empty()) service_.Release(id_);
            auto lifetime = std::move(rpc_lifetime_);
        }

        void OnWorkerEvent(ai::AgentWorkerEvent event)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_) return;
            if (event.kind == Kind::Joined)
            {
                if (stop_requested_ || writing_) return;
                joined_.set_participant_id(std::move(event.participant_id));
                writing_ = true;
                StartWrite(&joined_);
            }
            else if (unexpected_request_)
                Complete({grpc::StatusCode::INVALID_ARGUMENT, "Only one job is allowed per stream"});
            else if (event.kind == Kind::Failed)
                Complete({grpc::StatusCode::ABORTED, event.error});
            else Complete(grpc::Status::OK);
        }

        void StopRoom()
        {
            if (!std::exchange(stop_requested_, true)) service_.worker_.StopSession(id_);
        }

        void Complete(grpc::Status status)
        {
            status_ = std::move(status);
            if (!writing_) Finish(*status_);
        }

        WorkerService& service_;
        std::shared_ptr<Session> rpc_lifetime_;
        std::mutex mutex_;
        std::string id_;
        rpc::SessionJob request_;
        rpc::SessionJoined joined_;
        std::optional<grpc::Status> status_;
        bool writing_ = false;
        bool stop_requested_ = false;
        bool unexpected_request_ = false;
    };

public:
    WorkerService(ai::AgentWorker& worker, std::string token)
        : worker_(worker), token_(std::move(token)) {}

private:
    grpc::ServerBidiReactor<rpc::SessionJob, rpc::SessionJoined>* RunSession(
        grpc::CallbackServerContext* context) override
    {
        const auto& metadata = context->client_metadata();
        const auto token = metadata.find(ai::rpc_config::kTokenHeader);
        const bool authorized = token != metadata.end() && token->second.size() == token_.size() &&
            CRYPTO_memcmp(token->second.data(), token_.data(), token_.size()) == 0;
        return Session::Start(*this, authorized);
    }

    grpc::Status Claim(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.size() >= kMaxSessions)
            return {grpc::StatusCode::RESOURCE_EXHAUSTED, "Worker session capacity reached"};
        if (!active_.insert(id).second)
            return {grpc::StatusCode::ALREADY_EXISTS, "Worker session already exists"};
        return grpc::Status::OK;
    }

    void Release(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
    }

    ai::AgentWorker& worker_;
    const std::string token_;
    std::mutex mutex_;
    std::unordered_set<std::string> active_;
};
}

int main(int argc, char** argv)
{
    if (argc > 2)
    {
        std::cerr << "Usage: " << argv[0] << " [127.0.0.1:PORT]\n";
        return 2;
    }
    std::signal(SIGINT, StopSignal);
    std::signal(SIGTERM, StopSignal);
    try
    {
        const std::string address = argc == 2 ? argv[1] : ai::rpc_config::kDefaultAddress;
        ai::rpc_config::RequireLoopbackAddress(address);
        auto token = ai::rpc_config::Token();
        auto& logger = InitializeLogger("agent-worker");
        auto worker = ai::MakeLocalAgentWorker(ai::MakeAgentRoomClient());
        if (!worker->Start()) throw std::runtime_error("cannot start Agent room client");
        WorkerService service(*worker, std::move(token));
        grpc::ServerBuilder builder;
        builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
        builder.SetMaxReceiveMessageSize(16 * 1024);
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        if (!server) throw std::runtime_error("cannot bind Agent RPC listener");
        logger.Ready(__FILE__, __LINE__);
        logger.flush();
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server->Shutdown(std::chrono::system_clock::now());
        server->Wait();
        server.reset();
        worker->Stop();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Agent worker startup failed: " << error.what() << '\n';
        return 1;
    }
}
