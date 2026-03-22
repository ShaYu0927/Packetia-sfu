#ifndef  _RTSP_H_
#define  _RTSP_H_

#include <string>
#include <memory>


struct RtspUrlInfo
{
    std::string url;
	std::string ip;
	uint16_t port;
	std::string suffix;
};

enum RtspTransportType
{
    TcpInterleaved = 1,
    UdpInterleaved,
    Unknown
};

enum RtspMode
{
    ModeUnknown = 1,
    PLAY,
    RECORD
};

struct RtspTransport
{
    std::string lower_transport;    /* TCP / UDP */
    std::string profile;            /* RTP/AVP, RTP/AVP/TCP */
    bool unicast = true;
    bool multicast = false;

    int interleaved_rtp = -1;
    int interleaved_rtcp = -1;

    int client_rtp_port = -1;
    int client_rtcp_port = -1;

    int server_rtp_port = -1;
    int server_rtcp_port = -1;

    RtspTransportType transport = RtspTransportType::Unknown;
    RtspMode mode = RtspMode::ModeUnknown;
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

    virtual void SetVersion(const std::string& version)
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

    bool has_auth_info_;
	std::string realm_;
	std::string username_;
	std::string password_;
	std::string version_;
	struct RtspUrlInfo rtsp_url_info_;
};







#endif // _RTSP_H_