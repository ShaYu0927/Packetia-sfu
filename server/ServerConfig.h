#ifndef PACKETIA_SERVER_SERVERCONFIG_H_
#define PACKETIA_SERVER_SERVERCONFIG_H_

#include "service/RecordService/RecordingOptions.h"

#include <cstdint>
#include <string>

namespace server
{
struct ServerConfig
{
    std::string listen_ip = "0.0.0.0";
    std::uint16_t rtsp_port = 554;
    std::uint16_t sip_port = 5060;
    std::uint16_t rtmp_port = 1935;
    std::uint16_t udp_port = 9000;
    std::uint16_t websocket_port = 8080;
    std::uint32_t io_threads = 1;

    bool recording_enabled = true;
    service::RecordingOptions recording;

    // Apply the existing PACKETIA_RECORDING and PACKETIA_RECORD_DIR overrides.
    static ServerConfig FromEnvironment();
};
}

#endif // PACKETIA_SERVER_SERVERCONFIG_H_
