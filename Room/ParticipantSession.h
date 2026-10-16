#ifndef PACKETIA_ROOM_PARTICIPANT_SESSION_H_
#define PACKETIA_ROOM_PARTICIPANT_SESSION_H_

#include <cstdint>
#include <memory>
#include <utility>

namespace media { class SfuEndpoint; }

namespace room
{

enum class ParticipantSessionRole { Publish, Subscribe };

// Stable, protocol-independent room binding. The service owns negotiation,
// encryption, transport shutdown and registry cleanup separately.
class ParticipantSession final
{
public:
    using Ptr = std::shared_ptr<ParticipantSession>;

    ParticipantSession(uint64_t id, ParticipantSessionRole role,
                       std::shared_ptr<media::SfuEndpoint> endpoint)
        : id_(id), role_(role), endpoint_(std::move(endpoint)) {}

    uint64_t Id() const noexcept { return id_; }
    ParticipantSessionRole Role() const noexcept { return role_; }
    const std::shared_ptr<media::SfuEndpoint>& Endpoint() const noexcept { return endpoint_; }

private:
    const uint64_t id_;
    const ParticipantSessionRole role_;
    const std::shared_ptr<media::SfuEndpoint> endpoint_;
};

} // namespace room

#endif // PACKETIA_ROOM_PARTICIPANT_SESSION_H_
