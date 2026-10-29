#include "ParticipantMessages.h"

namespace server::signaling
{
room::ParticipantPermissions ParticipantPermissions(const service::auth::RoomGrant& grant)
{
    return {grant.can_publish, grant.can_subscribe, grant.room_admin, grant.can_update_own_metadata};
}

Json ParticipantMessage(const room::ParticipantInfo& info)
{
    const char* state = "joining";
    switch (info.state)
    {
    case room::ParticipantState::Joined: state = "joined"; break;
    case room::ParticipantState::Active: state = "active"; break;
    case room::ParticipantState::Disconnected: state = "disconnected"; break;
    case room::ParticipantState::Reconnecting: state = "reconnecting"; break;
    default: break;
    }
    const auto& permissions = info.permissions;
    return {{"participant_id", info.identity.participant_id}, {"subject_id", info.identity.subject_id},
        {"name", info.identity.name}, {"metadata", info.properties.metadata},
        {"attributes", info.properties.attributes}, {"state", state},
        {"permissions", {{"can_publish", permissions.can_publish}, {"can_subscribe", permissions.can_subscribe},
            {"room_admin", permissions.room_admin}, {"can_update_own_metadata", permissions.can_update_own_metadata}}}};
}

bool ApplyParticipantPatch(const Json& body, room::ParticipantInfo& info, Json& error)
{
    const auto reject = [&](const char* reason) { error = Error(reason); return false; };
    if (body.contains("name"))
    {
        if (!body["name"].is_string() || body["name"].get_ref<const std::string&>().size() > 256)
            return reject("name must be a string of at most 256 bytes");
        info.identity.name = body["name"].get<std::string>();
    }
    if (body.contains("metadata"))
    {
        if (!body["metadata"].is_string() || body["metadata"].get_ref<const std::string&>().size() > 4096)
            return reject("metadata must be a string of at most 4096 bytes");
        info.properties.metadata = body["metadata"].get<std::string>();
    }
    if (body.contains("attributes"))
    {
        const auto& attributes = body["attributes"];
        if (!attributes.is_object() || attributes.size() > 32)
            return reject("attributes must be an object with at most 32 keys");
        for (auto it = attributes.begin(); it != attributes.end(); ++it)
        {
            if (it.key().empty() || it.key().size() > 64 ||
                (!it->is_null() && (!it->is_string() || it->get_ref<const std::string&>().size() > 512)))
                return reject("Invalid attribute key or value");
            if (it->is_null()) info.properties.attributes.erase(it.key());
            else info.properties.attributes[it.key()] = it->get<std::string>();
        }
        std::size_t bytes = 0;
        for (const auto& attribute : info.properties.attributes) bytes += attribute.first.size() + attribute.second.size();
        if (info.properties.attributes.size() > 32 || bytes > 8192)
            return reject("Participant attributes exceed the size limit");
    }
    if (body.contains("permissions"))
    {
        const auto& permissions = body["permissions"];
        if (!permissions.is_object() || permissions.empty()) return reject("Expected a nonempty permissions object");
        for (auto it = permissions.begin(); it != permissions.end(); ++it)
        {
            if (!it->is_boolean()) return reject("Permissions must be boolean");
            const bool value = it->get<bool>();
            if (it.key() == "can_publish") info.permissions.can_publish = value;
            else if (it.key() == "can_subscribe") info.permissions.can_subscribe = value;
            else if (it.key() == "room_admin") info.permissions.room_admin = value;
            else if (it.key() == "can_update_own_metadata") info.permissions.can_update_own_metadata = value;
            else return reject("Unknown participant permission");
        }
    }
    return true;
}
}
