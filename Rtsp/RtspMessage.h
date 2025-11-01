#ifndef _RTSPMESSAGE_H_
#define _RTSPMESSAGE_H_

#include <string>
#include <array>
#include <unordered_map>
#include "BufferRead.h"

#include "logger.h"
#include "Rtp.h"
#include "Media.h"
#include "Rtsp.h"

#include "RtpReceiver.h"


class Sdp;



class RtspRequest
{
public:
    enum class Method
    {
        OPTIONS = 0,
        DESCRIBE,
        SETUP,
        PLAY,
        PAUSE,
        TEARDOWN,
        GET_PARAMETER,
        SET_PARAMETER,
        ANNOUNCE,
        RECORD,
        REDIRECT,
        RTCP,
        NONE
    };
    enum RtspRequestParseState
	{
		kParseRequestLine,
		kParseHeadersLine,
		kParseBody,	
		kGotAll,
        kParseDone
	};
    enum class Version
    {
        RTSP_1_0,
        RTSP_2_0
    };
   std::array<std::string, 3> ServerError = {
    "RTSP/1.0 500 Internal Server Error\r\n",
    "RTSP/1.0 404 Not Found\r\n",
    "RTSP/1.0 400 Bad Request\r\n"
    };



    Method GetMethodString(const char* method)
    {
        if(strcmp(method, "OPTIONS") == 0) return Method::OPTIONS;
        else if(strcmp(method, "DESCRIBE") == 0) return Method::DESCRIBE;
        else if(strcmp(method, "SETUP") == 0) return Method::SETUP;
        else if(strcmp(method, "PLAY") == 0) return Method::PLAY;
        else if(strcmp(method, "PAUSE") == 0) return Method::PAUSE;
        else if(strcmp(method, "TEARDOWN") == 0) return Method::TEARDOWN;
        else if(strcmp(method, "GET_PARAMETER") == 0) return Method::GET_PARAMETER;
        else if(strcmp(method, "SET_PARAMETER") == 0) return Method::SET_PARAMETER;
        else if(strcmp(method, "ANNOUNCE") == 0) return Method::ANNOUNCE;
        else if(strcmp(method, "RECORD") == 0) return Method::RECORD;
        else if(strcmp(method, "REDIRECT") == 0) return Method::REDIRECT;
        else if(strcmp(method, "$RTCP") == 0) return Method::RTCP;
        else return Method::NONE;

    }
    const char* GetVersionString(Version version)
    {
        switch (version)
        {
        case Version::RTSP_1_0: return "RTSP/1.0";
        case Version::RTSP_2_0: return "RTSP/2.0";
        default: return "";
        }
    }
    
    bool ParseRequest(BufferReader *buffer);
    

 
    Method GetMethod() const { return method_; }
    const std::string& GetMethodString() const { return method_str_; }
    Version GetVersion() const { return version_; }
    const MediaChannelId& GetSessionId()  { return session_id_; }
    const std::string& GetContentType()  { return content_type_; }
    const std::string& GetUserAgent()  { return user_agent_; }
    const std::string& GetAccept()  { return accept_; }
    const std::string& GetRange()  { return range_; }
    const RTPTransportMode GetTransport() ;
    const int GetContentLength();
    const int SetContentLength(int length);
    const std::string& GetControl()  { return trackId; }
    std::string GetRtspUSuffix() const;
    std::string GetCSeq() const
    {
        auto iter = header_line_param_.find("CSeq");
        if (iter != header_line_param_.end())
        {
            return iter->second.first;
        }
        return "";
    }

   
    const uint16_t GetRtpChannel()
    {
        return rtp_channel_;
    }

    const uint16_t GetRtcpChannel()
    {
        
        return rtcp_channel_;
    }
    const uint16_t GetRtpPort()
    {
        return rtp_port_;
    }
    const uint16_t GetRtcpPort()
    {
        return rtcp_port_;
    }

    void Reset()
    {
        //method_ = Method::NONE;
        state_ = RtspRequestParseState::kParseRequestLine;
        method_str_.clear();
        version_ = Version::RTSP_1_0;
        content_type_.clear();
        content_length_.clear();
        user_agent_.clear();
        accept_.clear();
        range_.clear();
        transport_.clear();
        authorization_.clear();
        rtsp_url_suffix_.clear();
        date_.clear();
        channel_id_ = channel_0;
        rtp_channel_ = 0;
        rtcp_channel_ = 0;
        rtp_port_ = 0;
        rtcp_port_ = 0;
        transport_mode_ = RTP_OVER_TCP;
    }
    std::string GetGmtTimeString();

    int BuildOptionsRes(std::shared_ptr<char> data,int size);
    int BuildDescribeRes(std::shared_ptr<char> data, int size, const std::string& sdp);
    int BuildSetupRes(std::shared_ptr<char> data, int size, uint16_t rtp_port, uint16_t rtcp_port, MediaChannelId channel_id,std::string session_id);
    int BuildNotFoundRes(std::shared_ptr<char> data,int size);
    int BuildServerErrorRes(std::shared_ptr<char> data, int size, const std::string& error_message);
    int BuildSetupMulticastRes(std::shared_ptr<char> data, int size, const char* multicast_ip, uint16_t port, uint32_t session_id);
    int BuildANNOUNCERes(std::shared_ptr<char> data, int size);
    int BuildRecordRes(std::shared_ptr<char> data, int size,std::string session_id);

private:
    Method method_;                                     //请求方法
    RtspRequestParseState state_;                       //解析状态
    std::string method_str_;                            //请求方法字符串
    std::string trackId;                                // SETUP中的trackID
    Version version_;                                   //版本
    MediaChannelId session_id_;                         //会话ID
    std::string content_type_;                          //内容类型
    std::string content_length_;                        //内容长度
    std::string user_agent_;                            //用户代理
    std::string accept_;                                //接受类型
    std::string range_;                                 //范围
    std::string transport_;                             //传输协议
    std::string authorization_;                         //授权
    std::string rtsp_url_suffix_;                       //RTSP URL后缀
    std::string date_;
    MediaChannelId channel_id_;
    std::string auth_response_;

    uint16_t rtp_channel_ = 0; //RTP端口
    uint16_t rtcp_channel_ = 0;

    uint16_t rtp_port_ = 0; //RTP端口
    uint16_t rtcp_port_ = 0;

    RTPTransportMode transport_mode_; //传输方式，默认为TCP

    bool ParseRequestLine(const char* begin, const char* end);
    bool ParseHeaderLines(const char*begin,const char* end);
    bool ParseBodyLine(const char*begin,const char* end);
    bool ParseCseq(const std::string& message);
    bool ParseSessionId(const std::string& line);
    bool ParseAccept(std::string& message);
    bool ParseTransport(const std::string &header);
    bool ParseMediaChannel(std::string& message);
	bool ParseAuthorization(std::string& message);

    std::unordered_map<std::string, std::pair<std::string, uint32_t>> request_line_param_;
    std::unordered_map<std::string, std::pair<std::string, uint32_t>> header_line_param_;

public:
    std::shared_ptr<Sdp> sdp_;


};


class RtspResponse
{

};

#endif