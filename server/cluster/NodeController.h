#pragma once
#include "ApiRouter.h"
#include "MeetingModule.h"
#include <map>
#include <memory>
namespace server::cluster
{
class NodeController final
{
public:
    NodeController(std::shared_ptr<service::meeting::MeetingModule> meetings, std::map<std::string, std::string> secrets)
        : meetings_(std::move(meetings)), secrets_(std::move(secrets)) {}
    void RegisterRoutes(api::ApiRouter::Builder& builder) const;
private:
    std::shared_ptr<service::meeting::MeetingModule> meetings_;
    std::map<std::string, std::string> secrets_;
};
}
