#ifndef _SDP_CFG_H_
#define _SDP_CFG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace sdp 
{

struct SdpLine
{
    char type = '\0';
    std::string value;
    std::string raw;
    int line_no = 0;
};

struct SdpAttribute
{
    std::string key;
    std::string value;
};

struct SdpConnection
{
    std::string net_type;
    std::string addr_type;
    std::string address;
};

struct SdpRtpMap
{
    int payloadType = -1;
    std::string encodingName;
    int clockRate = 0;
    int channels = 0;
};

struct SdpFmtp
{
    int payloadType = -1;
    std::string params;
};

enum class SdpProfile { Generic, WebRtc };

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

struct SdpMedia
{
    std::string media;                              
    int port = 0;
    std::string proto;                              
    std::vector<std::string> fmts;                  
    std::vector<SdpAttribute> attributes;
    SdpConnection conn;                             

    std::vector<SdpRtpMap> rtpmaps;
    std::vector<SdpFmtp> fmtps;

    std::string GetAttribute(const std::string& key) const;
    bool HasAttribute(const std::string& key) const;

    int portCount = 1; // Optional m=<media> <port>/<count>, primarily for RTSP.

    // WebRTC mode extracts known attributes into these fields. They are the
    // serialization authority; attributes retains unmodeled extensions only.
    std::string mid;
    MediaDirection direction = MediaDirection::SendRecv;
    bool rtcpMux = false;
    bool rtcpRsize = false;
    bool bundleOnly = false;

    // Effective transport parameters after applying session-level defaults.
    // Parsed media owns these values; edit each media when changing transport
    // parameters after parsing (session defaults are not live references).
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

struct SdpOrigin
{
    std::string username;
    std::string sess_id;
    std::string sess_version;
    std::string net_type;
    std::string addr_type;
    std::string unicast_address;
};


struct SdpSession
{
    int version = 0;
    SdpOrigin origin;                // o=
    std::string session_name;          // s=
    std::string connection;            // c=
    std::string timing;                // t=
    SdpConnection conn;

    std::vector<SdpAttribute> attributes;
    std::vector<SdpMedia> medias;

    SdpProfile profile = SdpProfile::Generic;
    // Offer/answer type is supplied by signaling, not encoded in the SDP text.
    SdpType type = SdpType::Offer;
    IceParameters ice;
    DtlsParameters dtls;
    BundleParameters bundle;
};



class ISdpFieldParser
{
public:
    virtual ~ISdpFieldParser() = default;
    virtual bool Parse(const SdpLine& line,
                       SdpSession& session,
                       SdpMedia*& current_media,
                       std::string& err) = 0;
};


}


#endif /* _SDP_CFG_H_ */
