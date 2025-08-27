#ifndef  _RTSP_H_
#define  _RTSP_H_

#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <map>
#include "MediaSession.h"
#include "logger.h"


class MediaSession;

struct RtspUrlInfo
{
    std::string url;
	std::string ip;
	uint16_t port;
	std::string suffix;
};

class Rtsp : public std::enable_shared_from_this<Rtsp> , public MediaSession
{
public:
    Rtsp() : has_auth_info_(false) {}
    virtual ~Rtsp() {}

    virtual void SetAuthInfo(const std::string& realm, const std::string& username, const std::string& password)
    {
        has_auth_info_ = true;
        realm_ = realm;
        username_ = username;
        password_ = password;
    }
    virtual bool HasAuthInfo() const { return has_auth_info_; }

    virtual void SetVersion(const std::string& version) // SDP Session Name
    {
        version_ = version;
    }
    
    virtual std::string GetRtspUrl()
	{ return rtsp_url_info_.url; }


    void ParseRtspUrl(std::string url)
    {
        size_t pos = url.find("://");
        if (pos != std::string::npos) {
            rtsp_url_info_.url = url.substr(pos + 3);
        } else {
            rtsp_url_info_.url = url;
        }

        pos = rtsp_url_info_.url.find('/');
        if (pos != std::string::npos) {
            rtsp_url_info_.suffix = rtsp_url_info_.url.substr(pos + 1);
            rtsp_url_info_.url = rtsp_url_info_.url.substr(0, pos);
        } else {
            rtsp_url_info_.suffix.clear();
        }

        pos = rtsp_url_info_.url.find(':');
        if (pos != std::string::npos) {
            rtsp_url_info_.ip = rtsp_url_info_.url.substr(0, pos);
            rtsp_url_info_.port = static_cast<uint16_t>(std::stoi(rtsp_url_info_.url.substr(pos + 1)));
        } else {
            rtsp_url_info_.ip = rtsp_url_info_.url;
            rtsp_url_info_.port = 554; // Default RTSP port
        }
    }


    virtual MediaSession::Ptr LookMediaSession(const std::string& suffix)
    {
        return nullptr; // Default implementation, should be overridden
    }

    virtual MediaSession::Ptr LookMediaSession(MediaSessionId sessionId)
	{ return nullptr; }

   bool AddMediaSession(const MediaSession::Ptr& session);

    bool has_auth_info_;
	std::string realm_;
	std::string username_;
	std::string password_;
	std::string version_;
	struct RtspUrlInfo rtsp_url_info_;
    std::map<std::string, MediaSession::Ptr> media_sessions_; // Maps suffix to MediaSession
};


/*
     v=0                              # SDP 协议版本
    o=- 0 0 IN IP4 192.168.0.1       # 会话发起者（用户名、session id、版本号、网络类型、地址类型、地址）
    s=SessionName                    # 会话名称
    c=IN IP4 192.168.0.1             # 连接信息
    t=0 0                            # 时间信息
    m=video 5004 RTP/AVP 96          # 媒体描述（如视频）
    a=rtpmap:96 H264/90000           # 媒体属性（编码方式）


*/
class Sdp // SDP (Session Description Protocol) class
{
public:
    using Ptr = std::shared_ptr<Sdp>;

    struct MediaDescription {
        std::string media_type;  // e.g., "video"
        int port;                // e.g., 5004
        std::string protocol;    // e.g., "RTP/AVP"
        int payload_type;        // e.g., 96
        std::string codec_name;  // e.g., "H264"
        int clock_rate;          // e.g., 90000
        std::string control;     // e.g., "trackID=0"
    };


    Sdp() = default;
    ~Sdp() {}

    std::string GetSdpMessage(const std::string& ip, const std::string& session_name = "Media Session");

    void AddMedia(const MediaDescription& media);

    std::vector<MediaDescription> media_list_;




};

class SdpTracker
{
public:
    typedef enum {
        TrackInvalid = -1,
        TrackVideo = 0,
        TrackAudio,
        TrackTitle,
        TrackApplication,
        TrackMax
    } TrackType;

    using Ptr = std::shared_ptr<SdpTracker>;

    std::string _t;
    std::string _b;
    uint16_t _port;

    float _duration = 0;
    float _start = 0;
    float _end = 0;

    std::map<char, std::string> _other;
    std::multimap<std::string, std::string> _attr;

    std::string toString(uint16_t port = 0) const;
    std::string getName() const;
    std::string getControlUrl(const std::string &base_url) const; //a=control:trackID=1


public:
    int _pt = 0xff;
    int _channel = 0;
    int _samplerate = 0;
    TrackType _type = TrackInvalid;
    std::string _codec;
    std::string _fmtp;
    std::string _control;

public:
    bool _inited = false;
    uint8_t _interleaved = 0;
    uint16_t _seq = 0;
    uint32_t _ssrc = 0;
    uint32_t _time_stamp = 0;

    SdpTracker() = default;
    virtual ~SdpTracker() {}

   

};


class SdpParser
{
public:
    using Ptr = std::shared_ptr<SdpParser>;

    SdpParser() = default;
    virtual ~SdpParser() {}

    void load(const std::string &sdp);
    bool available() const;
    SdpTracker::Ptr getTrack(SdpTracker::TrackType type) const;
    virtual bool Parse(const std::string& sdp_message, Sdp::Ptr& sdp);
    std::vector<SdpTracker::Ptr> getAvailableTrack() const;
    std::string toString() const;
    std::string getControlUrl(const std::string &url) const;

private:
    std::vector<SdpTracker::Ptr> _track_vec;
};




#endif // _RTSP_H_