#include "RtspMessage.h"

bool RtspRequest::ParseRequest(BufferReader *buffer)
{
    LOG_INFO("=== [RtspRequest] Begin parsing RTSP request from buffer ===");
    
    if (buffer->Peek()[0] == '$') // 判断是否 RTP OVER TCP
    {
        method_ = Method::RTCP;
        LOG_INFO("Detected RTP over TCP ('$' leading byte).");
        return true;
    }

    bool ret = true;
    while (true)
    {
        if (state_ == kParseRequestLine)
        {
            LOG_INFO("State: kParseRequestLine");

            const char* firstCrlf = buffer->FindFirstCrlf();
            if (!firstCrlf)
            {
                LOG_INFO("Waiting for more data to complete request line...");
                break;
            }

           std::string line(buffer->Peek(), firstCrlf - buffer->Peek());
            LOG_INFO("Request line: [" + line + "]");

            ret = ParseRequestLine(buffer->Peek(), firstCrlf);
            buffer->RetrieveUntil(firstCrlf + 2);

            if (!ret)
            {
                LOG_ERROR("Failed to parse request line.");
                return false;
            }

            LOG_INFO("Request line parsed successfully.");
        }
        else if (state_ == kParseHeadersLine)
        {
            LOG_INFO("State: kParseHeadersLine");

            const char* firstCrlf = buffer->FindFirstCrlf();
            if (!firstCrlf)
            {
                LOG_INFO("Waiting for more data to complete header line...");
                break;
            }

            std::string line(buffer->Peek(), firstCrlf - buffer->Peek());
            LOG_INFO("Header line: [" + line + "]");
            if(line == "CSeq")
            {
                header_line_param_["CSeq"] = std::make_pair(line, 0);
            }

            // 即使是空行，也应该先判断是否前面还有 header
            if (line.empty()) {
                buffer->RetrieveUntil(firstCrlf + 2);
                state_ = kParseDone;
                LOG_INFO("End of headers detected. State changed to kParseDone.");
                break;
            }

            ret = ParseHeaderLines(buffer->Peek(), firstCrlf);
            buffer->RetrieveUntil(firstCrlf + 2);

            if (!ret)
            {
                LOG_ERROR("Failed to parse header line.");
                return false;
            }

            LOG_INFO("Header line parsed successfully.");
        }
        else
        {
            LOG_INFO("State is not parsing. Breaking out of loop.");
            break;
        }
    }

    LOG_INFO("=== [RtspRequest] Finished parsing, state = " + std::to_string(state_) + " ===");
    return true;
}

const RTPTransportMode RtspRequest::GetTransport() const
{
    if(transport_mode_ == RTP_OVER_TCP)
    {
        return RTP_OVER_TCP;
    }
    else if(transport_mode_ == RTP_OVER_UDP)
    {
        return RTP_OVER_UDP;
    }
    else if(transport_mode_ == RTP_OVER_MULTICAST)
    {
        return RTP_OVER_MULTICAST;
    }
    return RTP_OVER_UNKNOWN;
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
    LOG_DEBUG("Building RTSP OPTIONS response with CSeq:" + this->GetCSeq());
   snprintf(data.get(), size,
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %s\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n"
        "\r\n",
        this->GetCSeq().c_str()
    );

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

int RtspRequest::BuildSetupRes(std::shared_ptr<char> data, int size, uint16_t rtp_port, uint16_t rtcp_port, MediaChannelId channel_id)
{
    memset((void*)data.get(), 0, size);
    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n";
    oss << "CSeq: " << this->GetCSeq() << "\r\n";
    oss << "Session: " << session_id_ << "\r\n";
    oss << "Transport: ";

    if (transport_mode_ == RTP_OVER_TCP) 
    {
        // RTP over TCP (interleaved方式)
        oss << "RTP/AVP/TCP;unicast;interleaved=" 
            << (channel_id * 2) << "-" << (channel_id * 2 + 1) << "\r\n";\
    }
    else if (transport_mode_ == RTP_OVER_UDP) 
    {
        // RTP over UDP
        oss << "RTP/AVP;unicast;client_port=" 
            << rtp_port << "-" 
            << rtcp_port << "\r\n";
    }
    else 
    {
        // 其他传输方式
        oss << "RTP/AVP;unicast\r\n";
    }
    std::string res = oss.str();
    if (res.size() > size) {
        // buffer不够，返回错误
        return -1;
    }

    memcpy(data.get(), res.c_str(), res.size());

    return res.size();
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

int RtspRequest::BuildSetupMulticastRes(std::shared_ptr<char> data, int size, const char *multicast_ip, uint16_t port, uint32_t session_id)
{
    return 0;
}

bool RtspRequest::ParseRequestLine(const char *begin, const char *end)
{
    LOG_INFO("Parsing RTSP request line: [" + std::string(begin, end) + "]");
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
    // LOG_INFO("Parsed method=[" + method_str_ + "], uri=[" + std::string(uri) + "], version=[" + std::string(version) + "]");

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

    LOG_INFO("URI without schema: [" + uri_str + "]");

    if (uri_str.find(" ") != std::string::npos ||
        uri_str.find("\r") != std::string::npos ||
        uri_str.find("\n") != std::string::npos ||
        uri_str.find("\t") != std::string::npos ||
        uri_str.find('\0') != std::string::npos)
    {
        LOG_ERROR("Invalid characters found in URI: [" + uri_str + "]");
        return false;
    }

    // 尝试提取 IP、端口、suffix（兼容无 suffix 的情况）
    uint16_t port = 0;
    char ip[64] = {0};
    char suffix[64] = {0};
    bool parse_ok = false;

    if (sscanf(uri_str.c_str(), "%63[^:]:%hu/%63s", ip, &port, suffix) == 3) {
        LOG_INFO("Parsed uri with port and suffix: ip=[" + std::string(ip) + "], port=[" + std::to_string(port) + "], suffix=[" + std::string(suffix) + "]");
        parse_ok = true;
    } else if (sscanf(uri_str.c_str(), "%63[^:]:%hu", ip, &port) == 2) {
        LOG_INFO("Parsed uri with port only: ip=[" + std::string(ip) + "], port=[" + std::to_string(port) + "]");
        suffix[0] = '\0';
        parse_ok = true;
    } else if (sscanf(uri_str.c_str(), "%63[^/]/%63s", ip, suffix) == 2) {
        port = 554;
        LOG_INFO("Parsed uri without port: ip=[" + std::string(ip) + "], suffix=[" + std::string(suffix) + "]");
        parse_ok = true;
    } else if (sscanf(uri_str.c_str(), "%63s", ip) == 1) {
        port = 554;
        suffix[0] = '\0';
        LOG_INFO("Parsed uri with only ip: ip=[" + std::string(ip) + "]");
        parse_ok = true;
    }

    if (!parse_ok) {
        LOG_ERROR("Failed to parse ip/port/suffix from uri: [" + uri_str + "]");
        return false;
    }

    // 填入参数
    request_line_param_.emplace("url", std::make_pair(uri_str, 0));
    request_line_param_.emplace("url_ip", std::make_pair(std::string(ip), 0));
    request_line_param_.emplace("url_port", std::make_pair("", static_cast<uint32_t>(port)));
    request_line_param_.emplace("url_suffix", std::make_pair(std::string(suffix), 0));
    request_line_param_.emplace("version", std::make_pair(std::string(version), 0));
    request_line_param_.emplace("method", std::make_pair(std::string(method), 0));

    state_ = kParseHeadersLine;
    LOG_INFO("RTSP request line parsed successfully.");
    return true;
}



bool RtspRequest::ParseHeaderLines(const char *begin, const char *end)
{
    std::string line(begin, end);
    LOG_INFO("Parsing RTSP header line: [" + line + "]");

    if (line.empty()) {
        LOG_INFO("Empty header line encountered");
        return false;
    }

    // 解析 CSeq
    if (line.find("CSeq:") == 0) {
        if (!ParseCseq(line)) {
            LOG_ERROR("CSeq parsing failed");
            return false;
        }
        return true;
    }

    // 解析 Accept
    if (method_ == Method::DESCRIBE && line.find("Accept:") == 0) {
        ParseAccept(line);
        return true;
    }

    // 解析 Transport
    if (method_ == Method::SETUP && line.find("Transport:") == 0) {
        LOG_INFO("Parsing SETUP Transport header: " + line);
        if (!ParseTransport(line)) {
            LOG_ERROR("Failed to parse Transport header");
            return false;
        }
        return true;
    }

    // 解析 Session
    if ((method_ == Method::PLAY || method_ == Method::TEARDOWN) && line.find("Session:") == 0) {
        if (!ParseSessionId(line)) {
            LOG_ERROR("Failed to parse Session header");
            return false;
        }
        return true;
    }

    // 解析 Authorization
    if (line.find("Authorization:") == 0) {
        ParseAuthorization(line);
        return true;
    }

    LOG_INFO("Unhandled or irrelevant header: " + line);
    return true;
}


bool RtspRequest::ParseCseq(const std::string &message)
{
    std::size_t pos = message.find("CSeq:");
    if(pos != std::string::npos)
    {
        uint32_t cseq = 0;
        if (sscanf(message.c_str() + pos, "%*[^:]: %u", &cseq) == 1)
        {
            header_line_param_.emplace("CSeq", std::make_pair(std::to_string(cseq), 0));
            return true;
        }
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

bool RtspRequest::ParseTransport(const std::string &header) {
    std::istringstream iss(header);
    std::string line;
    while (std::getline(iss, line)) {
        // 移除行尾 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.find("Transport:") != std::string::npos) {
            LOG_INFO("Parsing Transport line: " + line);

            // 判断 TCP
            if (line.find("interleaved=") != std::string::npos) {
                // RTP over TCP
                size_t pos = line.find("interleaved=");
                std::string channels = line.substr(pos + strlen("interleaved="));
                auto dash = channels.find('-');
                if (dash != std::string::npos) {
                    uint16_t rtp_channel = static_cast<uint16_t>(std::stoi(channels.substr(0, dash)));
                    uint16_t rtcp_channel = static_cast<uint16_t>(std::stoi(channels.substr(dash + 1)));

                    this->rtp_channel_ = rtp_channel;
                    this->rtcp_channel_ = rtcp_channel;
                    this->transport_mode_ = RTP_OVER_TCP;

                    LOG_INFO("Parsed interleaved channels: " + std::to_string(rtp_channel) + "-" + std::to_string(rtcp_channel));
                    return true;
                }
            }
            // 判断 UDP
            else if (line.find("client_port=") != std::string::npos) {
                size_t port_pos = line.find("client_port=");
                std::string ports = line.substr(port_pos + strlen("client_port="));
                auto dash = ports.find('-');
                if (dash != std::string::npos) {
                    uint16_t rtp_port = static_cast<uint16_t>(std::stoi(ports.substr(0, dash)));
                    uint16_t rtcp_port = static_cast<uint16_t>(std::stoi(ports.substr(dash + 1)));

                    this->rtp_port_ = rtp_port;
                    this->rtcp_port_ = rtcp_port;
                    this->transport_mode_ = RTP_OVER_UDP;

                    LOG_INFO("Parsed client ports: " + std::to_string(rtp_port) + "-" + std::to_string(rtcp_port));
                    return true;
                }
            }
            // 判断 Multicast
            else if (line.find("multicast") != std::string::npos) {
                this->transport_mode_ = RTP_OVER_MULTICAST;
                LOG_INFO("Parsed transport mode: Multicast");
                return true;
            }

            // 未识别
            LOG_ERROR("Transport line found but not parsed: " + line);
            return false;
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
