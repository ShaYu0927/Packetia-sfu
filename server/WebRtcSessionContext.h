#ifndef PACKETIA_SERVER_WEBRTC_SESSION_CONTEXT_H_
#define PACKETIA_SERVER_WEBRTC_SESSION_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace room { class ParticipantSession; }
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

namespace server
{

// WebRTC resources belong to the application service's owner scheduler.
// Room objects receive only the immutable binding, never this context.
struct WebRtcSessionContext
{
    uint64_t id = 0;
    std::string ufrag;
    std::shared_ptr<protocol::webrtc::WebRtcSession> rtc;
    std::shared_ptr<media::transport::WebRtcMediaTransport> transport;
    std::shared_ptr<media::transport::MediaEndpointIngress> ingress;
    std::shared_ptr<media::SfuEndpoint> endpoint;
    std::shared_ptr<room::ParticipantSession> room_binding;

    std::string pending_offer_id;
    uint64_t negotiation_deadline_ms = 0;
    bool endpoint_registered = false;
    bool room_managed = false;
    bool subscriber = false;
    // 仅记录本次媒体创建的房间资源，失败回滚不得移除已有参与者。
    std::vector<std::string> room_tracks;
    std::vector<std::string> room_subscriptions;
};

} // namespace server

#endif // PACKETIA_SERVER_WEBRTC_SESSION_CONTEXT_H_
