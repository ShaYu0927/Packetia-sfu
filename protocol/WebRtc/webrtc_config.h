#ifndef _WEBRTC_CONFIG_H_
#define _WEBRTC_CONFIG_H_

#include <string>
#include <vector>
#include <memory>

namespace RtpHeaderExtensionUri
{
    static const std::string SDES_MID =
        "urn:ietf:params:rtp-hdrext:sdes:mid";

    static const std::string SDES_RTP_STREAM_ID =
        "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id";

    static const std::string AUDIO_LEVEL =
        "urn:ietf:params:rtp-hdrext:ssrc-audio-level";

    static const std::string TRANSPORT_CC =
        "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

    static const std::string ABS_SEND_TIME =
        "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";

    static const std::string ABS_CAPTURE_TIME =
        "http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time";

    static const std::string FRAME_MARKING =
        "urn:ietf:params:rtp-hdrext:framemarking";

    static const std::string DEPENDENCY_DESCRIPTOR =
        "https://aomediacodec.github.io/av1-rtp-spec/#dependency-descriptor-rtp-header-extension";

    static const std::string REPAIRED_RTP_STREAM_ID =
        "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id";
}

namespace RtcpFeedbackType
{
    static const std::string NACK = "nack";
    static const std::string CCM = "ccm";
    static const std::string TRANSPORT_CC = "transport-cc";
    static const std::string GOOG_REMB = "goog-remb";
}

/*
 * RTCP Feedback parameter
 */
namespace RtcpFeedbackParameter
{
    static const std::string PLI = "pli";
    static const std::string FIR = "fir";
}

struct RtcpFeedback
{
    std::string type;
    std::string parameter;

    RtcpFeedback() = default;

    RtcpFeedback(const std::string& type_, const std::string& parameter_ = "")
        : type(type_), parameter(parameter_)
    {
    }
};

struct ReceiverConfig
{
    int packetBufferSizeVideo = 0;
    int packetBufferSizeAudio = 0;
};

struct RtpHeaderExtensionConfig
{
    std::vector<std::string> audio;
    std::vector<std::string> video;
};

struct RtcpFeedbackConfig
{
    std::vector<RtcpFeedback> audio;
    std::vector<RtcpFeedback> video;
};

struct DirectionConfig
{
    RtpHeaderExtensionConfig rtpHeaderExtension;
    RtcpFeedbackConfig rtcpFeedback;
};

struct CongestionControlConfig
{
    bool useSendSideBWEInterceptor = false;
    bool useSendSideBWE = false;
};

struct RTCConfig
{
    int packetBufferSize = 0;
    int packetBufferSizeVideo = 0;
    int packetBufferSizeAudio = 0;

    CongestionControlConfig congestionControl;
};

struct Config
{
    bool development = false;
    RTCConfig rtc;
};

/*
 * BufferFactory 占位类。
 *
 * 在实际实现中，BufferFactory 可以提供创建和管理 RTP/RTCP 数据包缓冲区的功能，以优化内存使用和性能。
 *
 */
class BufferFactory
{
public:
    BufferFactory() = default;
    virtual ~BufferFactory() = default;
};

/*
 * SettingEngine 占位类。
 *
 * 
 * webRTCConfig.SettingEngine.DisableActiveTCP(true)
 * c.SettingEngine.BufferFactory = factory.GetOrNew
 */
class SettingEngine
{
public:
    void disableActiveTCP(bool disable)
    {
        disableActiveTCP_ = disable;
    }

    bool isActiveTCPDisabled() const
    {
        return disableActiveTCP_;
    }

private:
    bool disableActiveTCP_ = false;
};
/*
 * WebRTC 基础配置。
 *
 */
struct BaseWebRTCConfig
{
    SettingEngine settingEngine;
};

class WebRTCConfig : public BaseWebRTCConfig
{
public:
    ReceiverConfig receiver;
    DirectionConfig publisher;
    DirectionConfig subscriber;

public:
    WebRTCConfig() = default;

    void updatePublisherConfig(bool consolidated);

    void updateSubscriberConfig(const CongestionControlConfig& ccConf);

    void setBufferFactory(std::shared_ptr<BufferFactory> factory);

    std::shared_ptr<BufferFactory> getBufferFactory() const;

private:
    std::shared_ptr<BufferFactory> bufferFactory_;
};

std::unique_ptr<WebRTCConfig> NewWebRTCConfig(Config conf);
DirectionConfig GetPublisherConfig(bool consolidated);
DirectionConfig GetSubscriberConfig(bool enableTWCC);


#endif // _WEBRTC_CONFIG_H_ 