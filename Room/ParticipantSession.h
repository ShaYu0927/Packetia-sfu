#ifndef PACKETIA_ROOM_PARTICIPANT_SESSION_H_
#define PACKETIA_ROOM_PARTICIPANT_SESSION_H_

#include <cstdint>
#include <memory>
#include <string>

namespace protocol::webrtc { class WebRtcSession; }
namespace media
{
class SfuEndpoint;
namespace transport
{
class WebRtcMediaTransport;
class MediaEndpointIngress;
}
}

namespace room
{

// Mutated only on the service's owner scheduler. Initialize ID and object
// handles before binding; keep them stable while attached to a participant.
// The service executes shutdown and registry cleanup on that same scheduler.
struct ParticipantSession
{
    using Ptr = std::shared_ptr<ParticipantSession>;

    uint64_t id = 0;
    std::string ufrag;
    std::shared_ptr<protocol::webrtc::WebRtcSession> rtc;
    std::shared_ptr<media::transport::WebRtcMediaTransport> transport;
    std::shared_ptr<media::transport::MediaEndpointIngress> ingress;
    std::shared_ptr<media::SfuEndpoint> endpoint;

    std::string pending_offer_id;
    uint64_t negotiation_deadline_ms = 0;
    bool endpoint_registered = false;
    bool room_managed = false;
    bool subscriber = false;
};

} // namespace room

#endif // PACKETIA_ROOM_PARTICIPANT_SESSION_H_
