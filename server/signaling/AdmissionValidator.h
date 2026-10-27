#pragma once
#include "SignalMessages.h"
#include "service/Auth/AuthService.h"
#include <functional>
#include <optional>

namespace server::signaling
{
struct ParticipantInit
{
    service::auth::AuthContext caller;
    std::string room_id;
    bool reconnect = false;
    std::string participant_id;
    std::string resume_token;
};
struct AdmissionValidation
{
    std::optional<ParticipantInit> init;
    Json error;
};
// 从原始凭证调用现有验签器；没有可复用的 WebSocket HTTP claims 中间件。
// 输出已验证的值快照，不创建房间、参与者或发送响应。
class AdmissionValidator
{
public:
    AdmissionValidator(std::shared_ptr<const service::auth::AuthService> auth,
                       std::function<bool(const service::auth::AuthContext&)> guard)
        : auth_(std::move(auth)), guard_(std::move(guard)) {}
    AdmissionValidation Validate(const SignalRequest& request) const;
private:
    std::shared_ptr<const service::auth::AuthService> auth_;
    std::function<bool(const service::auth::AuthContext&)> guard_;
};
}
