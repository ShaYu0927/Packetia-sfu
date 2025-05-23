#ifndef _RTSPMESSAGE_H_
#define _RTSPMESSAGE_H_

#include <string>
#include <unordered_map>
#include "BufferRead.h"
#include "Rtp.h"

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
    std::string date_;

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

#endif