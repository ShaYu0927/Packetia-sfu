#ifndef _WEBRTC_CONFIG_H_
#define _WEBRTC_CONFIG_H_

#include "SdpMode.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace protocol::webrtc
{

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


class BufferFactory
{
public:
    BufferFactory() = default;
    virtual ~BufferFactory() = default;
};


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

enum class SdpType
{
    Offer,
    Answer,
    Pranswer,
    Rollback
};

struct IceParameters
{
    std::string ufrag;
    std::string pwd;
    bool iceLite = false;
    std::vector<std::string> options;
    std::vector<std::string> candidates;
    bool endOfCandidates = false;
};

enum class DtlsSetup
{
    Unspecified,
    ActPass,
    Active,
    Passive,
    HoldConn
};

struct DtlsFingerprint
{
    std::string algorithm;
    std::string value;
};

struct DtlsParameters
{
    DtlsSetup setup = DtlsSetup::Unspecified;
    std::vector<DtlsFingerprint> fingerprints;
};

struct BundleParameters
{
    std::vector<std::string> mids;
};

enum class MediaDirection
{
    SendRecv,
    SendOnly,
    RecvOnly,
    Inactive
};

struct RtpCodecParameters
{
    int payloadType = -1;
    std::string encodingName;
    int clockRate = 0;
    int channels = 0;
    std::string fmtp;
    std::vector<RtcpFeedback> rtcpFeedback;
};

struct RtpHeaderExtensionParameters
{
    int id = 0;
    std::string uri;
    MediaDirection direction = MediaDirection::SendRecv;
    std::string attributes;
};

struct RtpSsrcParameters
{
    uint32_t ssrc = 0;
    std::vector<sdp::SdpAttribute> attributes;
};

struct RtpSsrcGroup
{
    std::string semantics;
    std::vector<uint32_t> ssrcs;
};

struct WebRtcMediaDescription
{
    // Original m= section, including media kind, port, protocol and attributes.
    sdp::SdpMedia sdp;
    std::string mid;
    MediaDirection direction = MediaDirection::SendRecv;
    bool rtcpMux = false;
    bool rtcpRsize = false;
    bool bundleOnly = false;

    // Effective transport parameters after applying session-level defaults.
    IceParameters ice;
    DtlsParameters dtls;

    std::vector<RtpCodecParameters> codecs;
    std::vector<RtpHeaderExtensionParameters> headerExtensions;
    // Feedback with a wildcard payload type (a=rtcp-fb:*).
    std::vector<RtcpFeedback> rtcpFeedback;
    std::vector<RtpSsrcParameters> ssrcs;
    std::vector<RtpSsrcGroup> ssrcGroups;
    std::vector<std::string> msids;
};

// Value model only: the raw SDP and extracted parameters are populated together
// by the caller; changing one representation does not update the other.
struct WebRtcSessionDescription
{
    SdpType type = SdpType::Offer;

    sdp::SdpSession sdp;

    IceParameters ice;
    DtlsParameters dtls;
    BundleParameters bundle;
    std::vector<WebRtcMediaDescription> medias;
};

std::unique_ptr<WebRTCConfig> NewWebRTCConfig(Config conf);
DirectionConfig GetPublisherConfig(bool consolidated);
DirectionConfig GetSubscriberConfig(bool enableTWCC);

}


#endif // _WEBRTC_CONFIG_H_
