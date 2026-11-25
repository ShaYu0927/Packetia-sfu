#pragma once
#include "ApiRouter.h"
#include "AccountService.h"
#include "MeetingModule.h"
#include "BusinessAccessPolicy.h"
namespace server::meeting
{
class RoomController final
{
public:
    RoomController(std::shared_ptr<service::account::AccountService> accounts,
                   std::shared_ptr<service::meeting::MeetingModule> meetings,
                   std::shared_ptr<const api::BusinessAccessPolicy> policy = std::make_shared<api::BusinessAccessPolicy>())
        : accounts_(std::move(accounts)), meetings_(std::move(meetings)),
          policy_(policy ? std::move(policy) : std::make_shared<api::BusinessAccessPolicy>()) {}
    void RegisterRoutes(api::ApiRouter::Builder& builder) const;
private:
    service::account::AccountIdentity ResolveCaller(const api::ApiRequest& request) const;
    protocol::http::HttpResponse CreateRoom(const api::ApiRequest& request) const;
    protocol::http::HttpResponse SetMemberRole(const api::ApiRequest& request) const;
    protocol::http::HttpResponse AdmitRoom(const api::ApiRequest& request) const;
    protocol::http::HttpResponse ManageParticipants(const api::ApiRequest& request, const std::string& type) const;
    protocol::http::HttpResponse EndRoom(const api::ApiRequest& request) const;

    std::shared_ptr<service::account::AccountService> accounts_;
    std::shared_ptr<service::meeting::MeetingModule> meetings_;
    std::shared_ptr<const api::BusinessAccessPolicy> policy_;
};
}
