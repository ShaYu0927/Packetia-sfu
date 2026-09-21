#ifndef PACKETIA_SERVER_SERVERAPP_H_
#define PACKETIA_SERVER_SERVERAPP_H_

#include "ServerConfig.h"
#include "ServerLauncher.h"

#include <memory>

class EventLoop;
namespace media { class EncodedFrameRouter; }

namespace server
{
// Owns shared resources and assembles services in dependency order.
class ServerApp final
{
public:
    explicit ServerApp(ServerConfig config);
    ~ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    bool Start();
    void Run();
    void Stop() noexcept;

private:
    void ConfigureServices();
    void AddMediaServices();
    void AddNetworkServices();

    ServerConfig config_;
    std::shared_ptr<EventLoop> event_loop_;
    std::shared_ptr<media::EncodedFrameRouter> frame_router_;
    // Destroy services before the resources referenced by their callbacks.
    ServerLauncher launcher_;
};
}

#endif // PACKETIA_SERVER_SERVERAPP_H_
