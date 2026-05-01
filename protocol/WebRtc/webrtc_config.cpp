#include "webrtc_config.h"

std::unique_ptr<WebRTCConfig> NewWebRTCConfig(Config conf)
{
    RTCConfig& rtcConf = conf.rtc;

    auto webRTCConfig = std::make_unique<WebRTCConfig>();
    webRTCConfig->settingEngine.disableActiveTCP(true);

    if (rtcConf.packetBufferSize == 0)
    {
        rtcConf.packetBufferSize = 500;
    }

    if (rtcConf.packetBufferSizeVideo == 0)
    {
        rtcConf.packetBufferSizeVideo = rtcConf.packetBufferSize;
    }

    if (rtcConf.packetBufferSizeAudio == 0)
    {
        rtcConf.packetBufferSizeAudio = rtcConf.packetBufferSize;
    }

    webRTCConfig->receiver.packetBufferSizeVideo = rtcConf.packetBufferSizeVideo;
    webRTCConfig->receiver.packetBufferSizeAudio = rtcConf.packetBufferSizeAudio;

    webRTCConfig->publisher = GetPublisherConfig(false);

    const bool enableTWCC =
        rtcConf.congestionControl.useSendSideBWEInterceptor ||
        rtcConf.congestionControl.useSendSideBWE;

    webRTCConfig->subscriber = GetSubscriberConfig(enableTWCC);

    return webRTCConfig;
}

void WebRTCConfig::updatePublisherConfig(bool consolidated)
{
    publisher = GetPublisherConfig(consolidated);
}

void WebRTCConfig::updateSubscriberConfig(const CongestionControlConfig& ccConf)
{
    const bool enableTWCC = ccConf.useSendSideBWEInterceptor || ccConf.useSendSideBWE;
    subscriber = GetSubscriberConfig(enableTWCC);
}

void WebRTCConfig::setBufferFactory(std::shared_ptr<BufferFactory> factory)
{
    bufferFactory_ = factory;

  
}

std::shared_ptr<BufferFactory> WebRTCConfig::getBufferFactory() const
{
    return bufferFactory_;
}

DirectionConfig GetPublisherConfig(bool consolidated)
{
    DirectionConfig config;

    config.rtpHeaderExtension.audio = {
        RtpHeaderExtensionUri::SDES_MID,
        RtpHeaderExtensionUri::SDES_RTP_STREAM_ID,
        RtpHeaderExtensionUri::AUDIO_LEVEL,
        RtpHeaderExtensionUri::ABS_CAPTURE_TIME,
    };

    if (consolidated)
    {
        config.rtpHeaderExtension.video = {
            RtpHeaderExtensionUri::SDES_MID,
            RtpHeaderExtensionUri::SDES_RTP_STREAM_ID,
            RtpHeaderExtensionUri::TRANSPORT_CC,
            RtpHeaderExtensionUri::ABS_SEND_TIME,
            RtpHeaderExtensionUri::FRAME_MARKING,
            RtpHeaderExtensionUri::DEPENDENCY_DESCRIPTOR,
            RtpHeaderExtensionUri::REPAIRED_RTP_STREAM_ID,
            RtpHeaderExtensionUri::ABS_CAPTURE_TIME,
        };

        config.rtcpFeedback.audio = {
            RtcpFeedback(RtcpFeedbackType::NACK),
        };

        config.rtcpFeedback.video = {
            RtcpFeedback(RtcpFeedbackType::TRANSPORT_CC),
            RtcpFeedback(RtcpFeedbackType::GOOG_REMB),
            RtcpFeedback(RtcpFeedbackType::CCM, RtcpFeedbackParameter::FIR),
            RtcpFeedback(RtcpFeedbackType::NACK),
            RtcpFeedback(RtcpFeedbackType::NACK, RtcpFeedbackParameter::PLI),
        };

        return config;
    }

    config.rtpHeaderExtension.video = {
        RtpHeaderExtensionUri::SDES_MID,
        RtpHeaderExtensionUri::SDES_RTP_STREAM_ID,
        RtpHeaderExtensionUri::TRANSPORT_CC,
        RtpHeaderExtensionUri::FRAME_MARKING,
        RtpHeaderExtensionUri::DEPENDENCY_DESCRIPTOR,
        RtpHeaderExtensionUri::REPAIRED_RTP_STREAM_ID,
        RtpHeaderExtensionUri::ABS_CAPTURE_TIME,
    };

    config.rtcpFeedback.audio = {
        RtcpFeedback(RtcpFeedbackType::NACK),
    };

    config.rtcpFeedback.video = {
        RtcpFeedback(RtcpFeedbackType::TRANSPORT_CC),
        RtcpFeedback(RtcpFeedbackType::CCM, RtcpFeedbackParameter::FIR),
        RtcpFeedback(RtcpFeedbackType::NACK),
        RtcpFeedback(RtcpFeedbackType::NACK, RtcpFeedbackParameter::PLI),
    };

    return config;

}

DirectionConfig GetSubscriberConfig(bool enableTWCC)
{
    DirectionConfig subscriberConfig;

    subscriberConfig.rtpHeaderExtension.video = {
        RtpHeaderExtensionUri::DEPENDENCY_DESCRIPTOR,
        RtpHeaderExtensionUri::ABS_CAPTURE_TIME,
    };

    subscriberConfig.rtpHeaderExtension.audio = {
        RtpHeaderExtensionUri::ABS_CAPTURE_TIME,
    };


    subscriberConfig.rtcpFeedback.audio = {
        RtcpFeedback(RtcpFeedbackType::NACK),
    };

    subscriberConfig.rtcpFeedback.video = {
        RtcpFeedback(RtcpFeedbackType::CCM, RtcpFeedbackParameter::FIR),
        RtcpFeedback(RtcpFeedbackType::NACK),
        RtcpFeedback(RtcpFeedbackType::NACK, RtcpFeedbackParameter::PLI),
    };

    if (enableTWCC)
    {
        subscriberConfig.rtpHeaderExtension.video.push_back(
            RtpHeaderExtensionUri::TRANSPORT_CC
        );

        subscriberConfig.rtcpFeedback.video.emplace_back(
            RtcpFeedbackType::TRANSPORT_CC
        );
    }
    else
    {
        subscriberConfig.rtpHeaderExtension.video.push_back(
            RtpHeaderExtensionUri::ABS_SEND_TIME
        );

        subscriberConfig.rtcpFeedback.video.emplace_back(
            RtcpFeedbackType::GOOG_REMB
        );
    }

    return subscriberConfig;
}


