#include "RtspMessage.h"

bool RtspRequest::ParseRequest(BufferReader *buffer)
{
    LOG_INFO("Parsing RTSP request from buffer");
    if(buffer->Peek()[0] == '$')  //判断是否RTP OVER TCP
    {
        method_ = Method::RTCP;
        return true;
    }

    bool ret = true;
    while(true)
    {
        if(state_ == kParseRequestLine)
        {
            const char* firstCrlf = buffer->FindFirstCrlf();
            if(!firstCrlf)
                break;  // 还没完整一行，等待更多数据

            LOG_INFO("First CRLF found at: " + std::to_string(firstCrlf - buffer->Peek()));
            ret = ParseRequestLine(buffer->Peek(), firstCrlf);
            buffer->RetrieveUntil(firstCrlf + 2);

            if(!ret)
                return false;
        }
        else if(state_ == kParseHeadersLine)
        {
            const char* firstCrlf = buffer->FindFirstCrlf();
            if(!firstCrlf)
                break;  // 还没完整一行，等待更多数据

            // 空行表示头部结束
            if(firstCrlf == buffer->Peek() || (firstCrlf[0] == '\r' && firstCrlf[1] == '\n'))
            {
                // 读取空行，结束头部解析
                buffer->RetrieveUntil(firstCrlf + 2);
                // 头部解析完成，改变状态
                state_ = kParseDone;  // 或者其他状态表示解析完毕
                break;
            }

            ret = ParseHeaderLines(buffer->Peek(), firstCrlf);
            buffer->RetrieveUntil(firstCrlf + 2);

            if(!ret)
                return false;
        }
        else
        {
            // 解析完成或者错误，跳出循环
            break;
        }
    }
    return true;
}


std::string RtspRequest::GetRtspUSuffix() const
{
    auto iter = request_line_param_.find("url_suffix");
    if (iter != request_line_param_.end())
    {
        return iter->second.first;
    }
    return "";
}

int RtspRequest::BuildOptionsRes(std::shared_ptr<char> data, int size)
{
    memset((void*)data.get(), 0, size);
    snprintf((char*)data.get(), size,
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %u\r\n"
			"Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n"
			"\r\n",
			this->GetCSeq());

	return (int)strlen(data.get());
}

int RtspRequest::BuildDescribeRes(std::shared_ptr<char> data, int size, const std::string &sdp)
{
    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n";
    oss << "CSeq: " << this->GetCSeq() << "\r\n";
    oss << "Content-Type: application/sdp\r\n";
    oss << "Content-Length: " << sdp.size() << "\r\n";
    oss << "\r\n";

     std::string res = oss.str() + sdp;

    // 检查是否能写入数据缓冲区
    if (res.size() > static_cast<size_t>(size)) {
        return -1; // buffer 太小
    }

    std::memcpy(data.get(), res.data(), res.size());
    return static_cast<int>(res.size());
}

int RtspRequest::BuildNotFoundRes(std::shared_ptr<char> data, int size)
{
    memset((void*)data.get(), 0, size);
	snprintf((char*)data.get(), size,
			"RTSP/1.0 404 Stream Not Found\r\n"
			"CSeq: %u\r\n"
			"\r\n",
			this->GetCSeq());

	return (int)strlen(data.get());
}

int RtspRequest::BuildServerErrorRes(std::shared_ptr<char> data, int size, const std::string &error_message)
{
    // 构造标准 RTSP 错误响应头
    std::ostringstream oss;
    oss << "RTSP/1.0 500 Internal Server Error\r\n";
    oss << "CSeq: " << this->GetCSeq() << "\r\n";  // 假设你有保存的 CSeq 值
    oss << "Content-Length: " << error_message.length() << "\r\n";
    oss << "Content-Type: text/plain\r\n";
    oss << "\r\n";
    oss << error_message;

    std::string res_str = oss.str();
    if (res_str.length() > static_cast<size_t>(size)) {
        return -1; // buffer 太小
    }

    memcpy(data.get(), res_str.c_str(), res_str.length());
    return static_cast<int>(res_str.length());
}


//OPTIONS rtsp://192.168.1.100:8554/live RTSP/1.0\r\n
bool RtspRequest::ParseRequestLine(const char *begin, const char *end)
{
    LOG_INFO("Parsing RTSP request line: " + std::string(begin, end));
    std::string request_line(begin, end);
    char method[16] = {0};
    char uri[256] = {0};
    char version[16] = {0};

    if (sscanf(request_line.c_str(), "%15s %255s %15s", method, uri, version) != 3)
        return false;

    method_ = GetMethodString(method);
    if (method_ == Method::NONE)
        return false;
    method_str_ = method;
    LOG_INFO("Parsed RTSP method: " + method_str_); 

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
    LOG_INFO("Parsing RTSP header lines: " + std::string(begin, end));
    std::string header_lines(begin, end);
    if(header_lines.empty())
    {
        LOG_ERROR("Header lines are empty");
        return false;
    }
    if(!ParseCseq(header_lines))
    {
       if(header_line_param_.find("CSeq") == header_line_param_.end())
       {
           LOG_ERROR("CSeq not found in header lines");
           return false;
       }
    }
    if(method_ == Method::DESCRIBE || method_ == Method::SETUP || method_ == Method::PLAY)
    {
        if(!ParseTransport(header_lines))
        {
            ParseAuthorization(header_lines);
        }
    }
   
    if(method_ == Method::OPTIONS) 
    {
		state_ = kGotAll;
		return true;
	}

    if(method_ == Method::DESCRIBE) 
    {
		if(ParseAccept(header_lines)) 
        {
			state_ = kGotAll;
		}
		return true;
	}

    if(method_ == Method::SETUP) 
    {
        if(ParseTransport(header_lines))
        {
            state_ = kGotAll;
        }
        return true;
    }

    if(method_ == Method::PLAY) 
    {
		if(ParseSessionId(header_lines)) 
        {
			state_ = kGotAll;
		}
		return true;
	}

    return true;
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
    channel_id_ = channel_0;

	auto iter = request_line_param_.find("url");
	if(iter != request_line_param_.end()) {
		std::size_t pos = iter->second.first.find("track1");
		if (pos != std::string::npos) {
			channel_id_ = channel_1;
		}       
	}

	return true;
}

bool RtspRequest::ParseAuthorization(std::string &message)
{
    std::size_t pos = message.find("Authorization:");
    if (pos != std::string::npos)
    {
        if(pos != std::string::npos)
        {
            if((message.find("resonse")) != std::string::npos)
            {
                auth_response_ = message.substr(pos + 10, 32);
			    if (auth_response_.size() == 32) 
                {
				    return true;
			    }
            }
        }
    }

    return false;
}
