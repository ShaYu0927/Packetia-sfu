#pragma once

#include "UdpSocket.h"
#include <cstdint>

namespace protocol::webrtc
{
// Server-side transport lifecycle. ICE restart offers still come from clients.
enum class WebRtcNetworkPathState { New, Checking, Ready, Failed, Closed };

struct WebRtcNetworkPath
{
    WebRtcNetworkPathState state = WebRtcNetworkPathState::New;
    uint64_t ice_generation = 0; // Initial start = 1; committed restarts increment.
    uint64_t path_revision = 0;  // Successful usable-path changes, including rebinds.
    // Present only in Ready. No ICE secrets are exposed through this interface.
    network::SocketAddr peer;
};
}
