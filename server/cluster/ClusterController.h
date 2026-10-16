#pragma once
#include "ApiRouter.h"
#include "AccountService.h"
#include "Coordinator.h"
#include <map>
#include <memory>

namespace server::cluster
{
class ClusterController final
{
public:
    ClusterController(std::shared_ptr<service::account::AccountService> accounts,
                      std::shared_ptr<service::cluster::Coordinator> coordinator,
                      std::map<std::string, std::string> node_secrets,
                      std::string issuer, std::string signing_secret);
    void RegisterPublic(server::api::ApiRouter::Builder& builder) const;
    void RegisterInternal(server::api::ApiRouter::Builder& builder) const;
private:
    struct State;
    std::shared_ptr<const State> state_;
};
}
