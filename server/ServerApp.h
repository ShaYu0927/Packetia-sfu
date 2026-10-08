#ifndef PACKETIA_SERVER_SERVERAPP_H_
#define PACKETIA_SERVER_SERVERAPP_H_

#include "ServerConfig.h"
#include "ServerLauncher.h"
#include "service/ConferenceMixService/ConferenceMixService.h"
#include "service/RecordService/RecordingRecords.h"

#include <memory>
#include <mutex>
#include <vector>

class EventLoop;
namespace media { class EncodedFrameRouter; }
namespace service { class RecordingService; class IRecordingEventSink; }
namespace sdp { struct SdpMedia; }

namespace server
{
class WebRtcService;
// Owns shared resources and assembles services in dependency order.
class ServerApp final
{
public:
    explicit ServerApp(ServerConfig config, std::shared_ptr<service::IRecordingEventSink> recording_events = {});
    ~ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    bool Start();
    void Run();
    void Stop() noexcept;

    bool SetServiceEnabled(service::ServiceType type, bool enabled);
    bool ServiceEnabled(service::ServiceType type) const;
    bool StartRecording(const config::StreamKey& stream);
    bool StopRecording(const config::StreamKey& stream);
    std::vector<service::SegmentInfo> QueryRecordingSegments(const service::SegmentQuery& query) const;
    std::vector<service::RecordedStream> ListRecordedStreams(const std::string& session_id) const;
    void SetStreamConfig(config::StreamKey stream, config::StreamOverrides overrides);
    void RemoveStreamConfig(const config::StreamKey& stream);
    void SetStreamDefaults(config::StreamFeatures defaults);
    config::StreamFeatures EffectiveStreamConfig(const config::StreamKey& stream) const;
    std::shared_ptr<const config::AppConfig> ConfigSnapshot() const;
    bool StartConference(service::mix::MixConfig config, std::unique_ptr<service::mix::IMixBackend> backend);
    bool StopConference(const std::string& room_id);
    bool RenegotiateWebRtc(uint64_t session_id, const std::vector<sdp::SdpMedia>& medias, std::string& error);
    // Output subscribers (such as recording) can be attached by the application.
    std::shared_ptr<media::EncodedFrameRouter> MixedFrameRouter() const { return mix_output_router_; }

private:
    void ConfigureServices();
    void AddMediaServices();
    void AddNetworkServices();
    void RefreshStreamPolicies();

    const ServerConfig config_; // Immutable network/resource startup settings.
    std::shared_ptr<config::ConfigStore> settings_;
    std::shared_ptr<service::IRecordingEventSink> recording_events_;
    mutable std::mutex lifecycle_mutex_;
    std::shared_ptr<EventLoop> event_loop_;
    std::shared_ptr<media::EncodedFrameRouter> frame_router_;
    std::shared_ptr<media::EncodedFrameRouter> mix_output_router_;
    std::shared_ptr<service::mix::ConferenceMixService> mix_service_;
    std::shared_ptr<service::RecordingService> recording_service_;
    std::shared_ptr<WebRtcService> webrtc_service_;
    // Destroy services before the resources referenced by their callbacks.
    ServerLauncher launcher_;
};
}

#endif // PACKETIA_SERVER_SERVERAPP_H_
