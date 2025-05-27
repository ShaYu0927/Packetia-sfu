#ifndef _RTSPMESSAGE_H_
#define _RTSPMESSAGE_H_

#include <string>
#include <unordered_map>
#include "BufferRead.h"
#include "logger.h"
#include "Rtp.h"
#include "Media.h"


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
		//kParseBody,	
		kGotAll,
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
    const std::string& GetSessionId() const { return session_id_; }
    const std::string& GetContentType() const { return content_type_; }
    const std::string& GetContentLength() const { return content_length_; }
    const std::string& GetUserAgent() const { return user_agent_; }
    const std::string& GetAccept() const { return accept_; }
    const std::string& GetRange() const { return range_; }
    const std::string& GetTransport() const { return transport_; }
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


    int BuildOptionsRes(std::shared_ptr<char> data,int size);
    int BuildDescribeRes(std::shared_ptr<char> data, int size, const std::string& sdp);
    int BuildNotFoundRes(std::shared_ptr<char> data,int size);
    int BuildServerErrorRes(std::shared_ptr<char> data, int size, const std::string& error_message);



private:
    Method method_;                                     //请求方法
    RtspRequestParseState state_;                       //解析状态
    std::string method_str_;                            //请求方法字符串
    Version version_;                                   //版本
    std::string session_id_;                            //会话ID
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

    bool ParseRequestLine(const char* begin, const char* end);
    bool ParseHeaderLines(const char*begin,const char* end);
    bool ParseCseq(const std::string& message);
    bool ParseSessionId(const std::string& line);
    bool ParseAccept(std::string& message);
    bool ParseTransport(std::string& message);
    bool ParseMediaChannel(std::string& message);
	bool ParseAuthorization(std::string& message);

    std::unordered_map<std::string, std::pair<std::string, uint32_t>> request_line_param_;
    std::unordered_map<std::string, std::pair<std::string, uint32_t>> header_line_param_;
};


class RtspResponse
{

};

#endif