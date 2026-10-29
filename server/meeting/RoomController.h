#pragma once
#include "ApiRouter.h"
#include "AccountService.h"
#include "MeetingModule.h"
namespace server::meeting
{
class RoomController final
{
public:
    RoomController(std::shared_ptr<service::account::AccountService> accounts,
                   std::shared_ptr<service::meeting::MeetingModule> meetings)
        : accounts_(std::move(accounts)), meetings_(std::move(meetings)) {}
    void RegisterRoutes(api::ApiRouter::Builder& builder) const;
private:
    std::shared_ptr<service::account::AccountService> accounts_;
    std::shared_ptr<service::meeting::MeetingModule> meetings_;
};
}
