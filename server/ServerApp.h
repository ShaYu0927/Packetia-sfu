#ifndef PACKETIA_SERVER_SERVERAPP_H_
#define PACKETIA_SERVER_SERVERAPP_H_

#include "ServerConfig.h"
#include "ServerLauncher.h"
#include "ThreadRuntimeStatus.h"
#include "MediaProcessingServices.h"
#include "RuntimeMetrics.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class EventLoop;
namespace media { class EncodedFrameRouter; }
namespace service { class MonitorService; }
namespace service::account { class AccountService; }
namespace sdp { struct SdpMedia; }
namespace protocol { class TurnServer; }

namespace server
{
class WebRtcService;
namespace api { class ApiServer; }
// Owns shared resources and assembles services in dependency order.
class ServerApp final
{
public:
    // Select processing implementations at construction; default to local services.
    explicit ServerApp(ServerConfig config,
        std::shared_ptr<service::IRecordingEventSink> recording_events = {},
        service::MediaProcessingFactory processing_factory = service::CreateLocalMediaProcessingServices);
    ~ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    bool Start();
    void Run();
    void Stop() noexcept;
    ThreadRuntimeStatus ThreadStatus() const;

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
    std::shared_ptr<media::EncodedFrameRouter> MixedFrameRouter() const { return processing_.mixed_frames; }
    bool StartTranscode(service::transcode::TranscodeTaskConfig config,
                        std::function<void()> request_keyframe = {});
    bool StopTranscode(const std::string& task_id, service::transcode::TranscodeTaskStatus* final_status = nullptr);
    std::optional<service::transcode::TranscodeTaskStatus> TranscodeStatus(const std::string& task_id) const;
    std::string TranscodeLastError() const;
    std::shared_ptr<media::EncodedFrameRouter> TranscodedFrameRouter() const { return processing_.transcoded_frames; }

private:
    void ConfigureServices();
    void AddAccountServices();
    void AddApiService();
    void AddMediaServices();
    void AddNetworkServices();
    void AddTurnService();
    void RefreshStreamPolicies();

    monitoring::MetricsContext metrics_context_{false};
    std::shared_ptr<service::MonitorService> monitor_service_;
    const ServerConfig config_; // Immutable network/resource startup settings.
    std::shared_ptr<config::ConfigStore> settings_;
    mutable std::mutex lifecycle_mutex_;
    std::shared_ptr<EventLoop> event_loop_;
    std::shared_ptr<service::account::AccountService> account_service_;
    std::shared_ptr<api::ApiServer> api_server_;
    std::shared_ptr<media::EncodedFrameRouter> frame_router_;
    service::MediaProcessingServices processing_;
    std::shared_ptr<WebRtcService> webrtc_service_;
    std::shared_ptr<protocol::TurnServer> turn_server_;
    // Destroy services before the resources referenced by their callbacks.
    ServerLauncher launcher_;
};
}

#endif // PACKETIA_SERVER_SERVERAPP_H_
