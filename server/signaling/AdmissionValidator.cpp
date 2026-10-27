#include "AdmissionValidator.h"

namespace server::signaling
{
AdmissionValidation AdmissionValidator::Validate(const SignalRequest& request) const
{
    namespace auth = service::auth;
    const auto& body = request.body;
    if (!body.contains("token") || !body["token"].is_string())
        return {{}, AuthError(auth::AuthError::Unauthenticated)};
    auto verified = auth_->Authenticate(body["token"].get_ref<const std::string&>(), auth::Audience::WebRtc);
    if (!verified.Succeeded()) return {{}, AuthError(verified.error)};
    if (guard_ && !guard_(*verified.context)) return {{}, AuthError(auth::AuthError::Forbidden)};
    if (!body.contains("room_id") || !body["room_id"].is_string()) return {{}, Error("Expected room_id")};
    auto room = body["room_id"].get<std::string>();
    const auto decision = auth_->Authorize(*verified.context, auth::Action::JoinRoom, RoomResource(room));
    if (!decision.Allowed()) return {{}, AuthError(decision.error)};
    if (room.empty() || room.size() > 64 ||
        room.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos)
        return {{}, Error("Invalid room_id")};
    ParticipantInit init;
    init.caller = std::move(*verified.context);
    init.room_id = std::move(room);
    init.reconnect = request.type == SignalRequest::Type::Resume;
    if (init.reconnect)
    {
        if (!body.contains("participant_id") || !body["participant_id"].is_string() ||
            !body.contains("resume_token") || !body["resume_token"].is_string())
            return {{}, Error("Expected room, participant and resume token")};
        init.participant_id = body["participant_id"].get<std::string>();
        init.resume_token = body["resume_token"].get<std::string>();
    }
    return {std::move(init), {}};
}
}
