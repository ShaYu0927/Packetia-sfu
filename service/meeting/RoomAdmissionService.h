#pragma once
#include "MeetingService.h"
#include "Coordinator.h"
#include "AuthTypes.h"
namespace service::meeting
{
struct RoomAdmission
{
    cluster::RoomAssignment assignment;
    std::string token;
    int64_t expires_at = 0;
};
class RoomAdmissionService final
{
public:
    RoomAdmissionService(std::shared_ptr<MeetingService> meetings, std::shared_ptr<cluster::Coordinator> nodes,
                         std::string issuer, std::string secret)
        : meetings_(std::move(meetings)), nodes_(std::move(nodes)), issuer_(std::move(issuer)), secret_(std::move(secret)) {}
    RoomAdmission Admit(const std::string& room, const std::string& user, const std::string& name);
    RoomAdmission AdmitAgent(const std::string& room, const std::string& owner, const std::string& agent);
private:
    RoomAdmission Issue(const std::string& subject, const std::string& name, auth::RoomGrant grant);
    std::shared_ptr<MeetingService> meetings_;
    std::shared_ptr<cluster::Coordinator> nodes_;
    std::string issuer_, secret_;
};
}
