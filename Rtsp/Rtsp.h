#ifndef  _RTSP_H_
#define  _RTSP_H_

#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <map>

#include "logger.h"
#include "Media.h"
#include "Rtp.h"
#include "MediaSession.h"


class MediaSession;
class SdpTracker;

struct RtspUrlInfo
{
    std::string url;
	std::string ip;
	uint16_t port;
	std::string suffix;
};

class Rtsp : public std::enable_shared_from_this<Rtsp>
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
        if (pos != std::string::npos) 
        {
            rtsp_url_info_.url = url.substr(pos + 3);
        } 
        else 
        {
            rtsp_url_info_.url = url;
        }

        pos = rtsp_url_info_.url.find('/');
        if (pos != std::string::npos) 
        {
            rtsp_url_info_.suffix = rtsp_url_info_.url.substr(pos + 1);
            rtsp_url_info_.url = rtsp_url_info_.url.substr(0, pos);
        } 
        else 
        {
            rtsp_url_info_.suffix.clear();
        }

        pos = rtsp_url_info_.url.find(':');
        if (pos != std::string::npos) 
        {
            rtsp_url_info_.ip = rtsp_url_info_.url.substr(0, pos);
            rtsp_url_info_.port = static_cast<uint16_t>(std::stoi(rtsp_url_info_.url.substr(pos + 1)));
        } 
        else 
        {
            rtsp_url_info_.ip = rtsp_url_info_.url;
            rtsp_url_info_.port = 554; // Default RTSP port
        }
    }


    std::shared_ptr<MediaSession> LookMediaSession(const std::string& suffix);
    std::shared_ptr<MediaSession> LookMediaSession(MediaSessionId sessionId);
    bool AddMediaSession(const std::shared_ptr<MediaSession>& session);

    bool has_auth_info_;
	std::string realm_;
	std::string username_;
	std::string password_;
	std::string version_;
	struct RtspUrlInfo rtsp_url_info_;
    std::map<std::string, std::shared_ptr<MediaSession>> media_sessions_;
};


class SdpTracker 
{
public:
    typedef enum 
    {
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
    int _clock_rate ;
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


/*
     v=0                              # SDP 协议版本
    o=- 0 0 IN IP4 192.168.0.1       # 会话发起者（用户名、session id、版本号、网络类型、地址类型、地址）
    s=SessionName                    # 会话名称
    c=IN IP4 192.168.0.1             # 连接信息
    t=0 0                            # 时间信息
    m=video 5004 RTP/AVP 96          # 媒体描述（如视频）
    a=rtpmap:96 H264/90000           # 媒体属性（编码方式）


*/
class Sdp : public std::enable_shared_from_this<Sdp> // SDP (Session Description Protocol) class
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
        std::string sprop_ps;
        std::string ssp;
    };


    Sdp() = default;
    ~Sdp() {}

    std::string GetSdpMessage(const std::string& ip, const std::string& session_name = "Media Session");

    void AddMedia(const MediaDescription& media);

    int GetMediaCount() const { return static_cast<int>(media_list_.size()); }
    int GetControlId(int index) const 
    {
        if (index < 0 || index >= static_cast<int>(media_list_.size())) 
        {
            return -1; // Invalid index
        }
        const std::string& control = media_list_[index].control;
        size_t pos = control.find("trackID=");
        if (pos != std::string::npos) 
        {
            return std::stoi(control.substr(pos + 8));
        }
        return -1; // Not found
    }

    virtual RtspSessionDesc parse(const std::string &sdp);

    //媒体列表
    std::vector<MediaDescription> media_list_;
    RtspSessionDesc sessionDesc;

    virtual void Parse();

    std::vector<SdpTracker::Ptr> tracks;

    void AddTrack(const SdpTracker::Ptr& track) 
    {
        tracks.push_back(track);
    }

    SdpTracker::Ptr GetTrack(int index) const 
    {
        if (index < 0 || index >= (int)tracks.size())
            return nullptr;
        return tracks[index];
    }
    std::string buildANNOUNCEBody();
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