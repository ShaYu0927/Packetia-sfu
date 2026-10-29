#pragma once
#include "ApiRouter.h"
#include "Coordinator.h"
#include "NodeLease.h"
#include "WebRtcService.h"
namespace server::meeting
{
class RoomControlController final
{
public:
    RoomControlController(service::cluster::Node node, std::string credential,
                          std::shared_ptr<service::cluster::NodeLease> lease, std::shared_ptr<WebRtcService> rtc)
        : node_(std::move(node)), credential_(std::move(credential)), lease_(std::move(lease)), rtc_(std::move(rtc)) {}
    void RegisterRoutes(api::ApiRouter::Builder& builder) const;
private:
    service::cluster::Node node_;
    std::string credential_;
    std::shared_ptr<service::cluster::NodeLease> lease_;
    std::shared_ptr<WebRtcService> rtc_;
};
}
