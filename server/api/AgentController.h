#pragma once

#include "ApiRouter.h"
#include "AccountService.h"
#include "VoiceAgentService.h"

namespace server::api
{
class AgentController final
{
public:
    AgentController(std::shared_ptr<service::account::AccountService> accounts,
                    std::shared_ptr<service::ai::VoiceAgentService> agents)
        : accounts_(std::move(accounts)), agents_(std::move(agents)) {}
    void RegisterRoutes(ApiRouter::Builder& builder) const;

private:
    std::string ResolveOwner(const ApiRequest& request) const;
    protocol::http::HttpResponse Create(const ApiRequest& request) const;
    protocol::http::HttpResponse Get(const ApiRequest& request) const;
    protocol::http::HttpResponse Stop(const ApiRequest& request) const;

    std::shared_ptr<service::account::AccountService> accounts_;
    std::shared_ptr<service::ai::VoiceAgentService> agents_;
};
}
