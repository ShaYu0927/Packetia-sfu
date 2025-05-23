#include "RtspMessage.h"

bool RtspRequest::ParseRequest(BufferReader *buffer)
{
    if(buffer->Peek()[0] == '$')  //判读是否RTP OVER TCP
    {
        method_ = Method::RTCP;
        return true;
    }

    bool ret = true;
    while(1)
    {
        if(state_ == kParseRequestLine)
        {
            const char* firstCtrlf = buffer->FindFirstCrlf();
            if(!firstCtrlf)
            {
                ret = ParseRequestLine(buffer->Peek(), firstCtrlf);
				buffer->RetrieveUntil(firstCtrlf + 2);
            }
        }
        if(state_ == kParseHeadersLine)
        {
            const char* firstCrlf = buffer->FindFirstCrlf();
            if(!firstCrlf)
            {
                ret = ParseHeaderLines(buffer->Peek(), firstCrlf);
                buffer->RetrieveUntil(firstCrlf + 2);
            }
        }
        else
        {
            break; 
        }
    }
    return true;
}

//OPTIONS rtsp://192.168.1.100:8554/live RTSP/1.0\r\n
bool RtspRequest::ParseRequestLine(const char *begin, const char *end)
{
    std::string request_line(begin, end);
    char method[16] = {0};
    char uri[256] = {0};
    char version[16] = {0};

    if (sscanf(request_line.c_str(), "%15s %255s %15s", method, uri, version) != 3)
        return false;

    method_ = GetMethodString(method);
    if (method_ == Method::NONE)
        return false;

    if (strncmp(version, "RTSP/1.0", 8) == 0)
        version_ = Version::RTSP_1_0;
    else if (strncmp(version, "RTSP/2.0", 8) == 0)
        version_ = Version::RTSP_2_0;
    else
        return false;

    std::string uri_str;
    if (strncmp(uri, "rtsp://", 7) == 0)
        uri_str = uri + 7;
    else
        uri_str = uri;

    if (uri_str.find(" ") != std::string::npos ||
        uri_str.find("\r\n") != std::string::npos ||
        uri_str.find("\n") != std::string::npos ||
        uri_str.find("\r") != std::string::npos ||
        uri_str.find("\t") != std::string::npos ||
        uri_str.find('\0') != std::string::npos)
    {
        return false;
    }

    // 提取 IP、端口、后缀
    uint16_t port = 0;
    char ip[64] = {0};
    char suffix[64] = {0};

    if (sscanf(uri_str.c_str(), "%[^:]:%hu/%s", ip, &port, suffix) != 3) {
        if (sscanf(uri_str.c_str(), "%[^/]/%s", ip, suffix) == 2) {
            port = 554;
        } else {
            return false;
        }
    }

    request_line_param_.emplace("url", std::make_pair(uri_str, 0));
    request_line_param_.emplace("url_ip", std::make_pair(std::string(ip), 0));
    request_line_param_.emplace("url_port", std::make_pair("", static_cast<uint32_t>(port)));
    request_line_param_.emplace("url_suffix", std::make_pair(std::string(suffix), 0));
    request_line_param_.emplace("version", std::make_pair(std::string(version), 0));
    request_line_param_.emplace("method", std::make_pair(std::string(method), 0));

    state_ = kParseHeadersLine;
    return true;
}


bool RtspRequest::ParseHeaderLines(const char *begin, const char *end)
{
    return false;
}

bool RtspRequest::ParseCseq(const std::string &message)
{
    std::size_t pos = message.find("CSeq:");
    if(pos != std::string::npos)
    {
        uint32_t cseq = 0;
        sscanf(message.c_str() + pos, "%*[^:]: %u", &cseq);
        header_line_param_.emplace("CSeq", std::make_pair(std::to_string(cseq), 0));
    }
    return false;
}

bool RtspRequest::ParseSessionId(const std::string &line)
{
    return false;
}

bool RtspRequest::ParseAccept(std::string &message)
{
    if(message.find("application/sdp") != std::string::npos)
    {
        header_line_param_.emplace("Accept", std::make_pair("application/sdp", 0));
        return true;
    }
    else if (message.find("application/x-rtsp-tunnelled") != std::string::npos)
    {
        header_line_param_.emplace("Accept", std::make_pair("application/x-rtsp-tunnelled", 0));
        return true;
    }
    return false;
}

//解析 RTSP 中的 Transport 头部，提取传输方式（TCP/UDP/Multicast）和对应的 端口 或 通道号。
bool RtspRequest::ParseTransport(std::string &message)
{
    size_t pos = message.find("Transport:");
    if (pos != std::string::npos)
    {
       if((pos = message.find("RTP/AVP/TCP")) != std::string::npos)
        {
            transport_ = RTP_OVER_TCP;
			uint16_t rtpChannel = 0, rtcpChannel = 0;
            return true;
        }
        else if((pos = message.find("RTP/AVP/UDP")) != std::string::npos)
        {
            header_line_param_.emplace("Transport", std::make_pair("RTP/AVP/UDP", 0));
            return true;
        }
        else if((pos = message.find("RTP/AVP/MULTICAST")) != std::string::npos)
        {
            header_line_param_.emplace("Transport", std::make_pair("RTP/AVP/MULTICAST", 0));
            return true;
        }
    }
    return false;
}

bool RtspRequest::ParseMediaChannel(std::string &message)
{
    return false;
}

bool RtspRequest::ParseAuthorization(std::string &message)
{
    return false;
}
