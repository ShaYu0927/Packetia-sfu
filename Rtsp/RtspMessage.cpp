#include "RtspMessage.h"
#include "MediaSession.h"

bool RtspRequest::ParseRequest(BufferReader *buffer)
{
    // 底层读取socket没有数据，需要切割
    LOG_INFO("ReadableBytes=" + std::to_string(buffer->ReadableBytes()));
    if (buffer->Peek()[0] == '$') // 判断是 RTP 数据包
    {
        LOG_INFO("Detected RTP over TCP ('$' leading byte). Skip RTSP parse.");
         const char* p = buffer->Peek();

        uint8_t dollar = static_cast<uint8_t>(p[0]);
        if (dollar != '$') {
            LOG_ERROR("Expected '$' at the start of interleaved packet.");
            return false;
        }
        
        uint8_t channel = static_cast<uint8_t>(p[1]);  // 增加channel映射
        auto tracker = MediaSessionManager::Instance().GetTcpChannelByChannel(channel);
        LOG_INFO("Interleaved packet on channel: " + std::to_string(channel) +
                 " mapped to tracker: " + std::to_string(tracker));
        uint16_t length  = (static_cast<uint8_t>(p[2]) << 8) | static_cast<uint8_t>(p[3]);
        // 跳过 4 字节头 + length 数据
        if (buffer->ReadableBytes() >= length + 4) 
        {
            if(p[0] != '$')
            {
                LOG_ERROR("Expected '$' at the start of interleaved packet.");
                return false;
            }

            LOG_INFO("Processing RTP packet for channel: " + std::to_string(channel) +
                     " with length: " + std::to_string(length));
            /* bug:tracker_ptr is null or already released */
            auto tracker_ptr = MediaSessionManager::Instance().GetTrackByChnnel(tracker);
            if (tracker_ptr) 
            {
                auto ptr = (uint8_t*)buffer->Peek() + 4;
                static_assert(std::is_same_v<decltype(tracker_ptr), std::shared_ptr<RtpTrack>>,
                    "tracker_ptr type mismatch");
                tracker_ptr->inputRtp(tracker_ptr->getType(), tracker_ptr->getSampleRate(), ptr, length);

                buffer->Retrieve(length + 4);
            } 
            else 
            {
                LOG_ERROR("No tracker found for channel: " + std::to_string(channel));
            }

            LOG_INFO("RTP packet processed for channel: " + std::to_string(channel));
            return true;
        } 
        else 
        {
            LOG_INFO("Incomplete interleaved packet, waiting for more data.");
            return false;
        }

        
        return false; // 表示这不是 RTSP 请求，直接返回
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

            //存储CSeq
            if (line.find("CSeq:") != std::string::npos)
            {
                ParseCseq(line);
            }

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
            //LOG_INFO("State: kParseHeadersLine");

            const char* firstCrlf = buffer->FindFirstCrlf();
            if (!firstCrlf)
            {
                LOG_INFO("Waiting for more data to complete header line...");
                break;
            }

            std::string line(buffer->Peek(), firstCrlf - buffer->Peek());
            LOG_INFO("Header line: [" + line + "]");

             // 即使是空行，也应该先判断是否前面还有 header
            if (line.empty()) 
            {
                buffer->RetrieveUntil(firstCrlf + 2);
                int content_len = GetContentLength();
                LOG_INFO("End of headers detected. Content-Length: " + std::to_string(content_len));
                if (header_line_param_.count("Content-Length") > 0)
                {
                    state_ = kParseBody;  // 如果有 body，就进入 kParseBody
                    LOG_INFO("End of headers detected. Content-Length present, next state = kParseBody.");
                }
                else
                {
                    state_ = kParseDone;  // 没有 body，直接结束
                    LOG_INFO("End of headers detected. No body, state changed to kParseDone.");
                    break;
                }
                continue;
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
        else if (state_ == kParseBody)
        {
            LOG_INFO("State: kParseBody");
            LOG_INFO("Before parsing body, ReadableBytes=" + std::to_string(buffer->ReadableBytes()));
            //LOG_INFO("Peek first 20 bytes: [" + std::string(buffer->Peek(), std::min(20, buffer->ReadableBytes())) + "]");

            int content_len = GetContentLength();
            LOG_INFO("Expecting body of length: " + std::to_string(content_len));
            if (content_len == 0) 
            {
                state_ = kParseRequestLine;
                break;
            }
            if (content_len > 0 && buffer->ReadableBytes() >= content_len) {
                ret = ParseBodyLine(buffer->Peek(), buffer->Peek() + content_len);
                buffer->Retrieve(content_len);
                SetContentLength(0); // Reset Content-Length after reading body
                state_ = kParseDone;
            }
            else 
            {
                LOG_INFO("Waiting for more data to complete body...");
                state_ == kParseBody;
                break;
            }

        }
        else if (state_ == kParseDone)
        {
            LOG_INFO("State: kParseDone. Parsing complete.");
            state_ = RtspRequestParseState::kParseRequestLine;
            //Reset();
            break;
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



RTPTransportMode RtspRequest::GetTransport()
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

//Content-Length: 561
int RtspRequest::GetContentLength()
{
    auto iter = header_line_param_.find("Content-Length");
    if (iter != header_line_param_.end())
    {
        return std::stoi(iter->second.first);
    }
    return 0;
}

int RtspRequest::SetContentLength(int length)
{
    content_length_ = std::to_string(length);
    header_line_param_["Content-Length"] = std::make_pair(content_length_, false);
    return 0;
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

std::string RtspRequest::GetGmtTimeString()
{
    char buf[128];
    time_t now = time(nullptr);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_now);
    return buf;
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

int RtspRequest::BuildSetupRes(std::shared_ptr<char> data, int size, uint16_t rtp_port, uint16_t rtcp_port, MediaChannelId channel_id,std::string session_id)
{
    memset((void*)data.get(), 0, size);

     std::stringstream randisss;
    randisss << std::hex << (rand() & 0xFFFFFFFF);

    std::ostringstream oss;
    oss << "RTSP/1.0 200 OK\r\n";
    oss << "CSeq: " << this->GetCSeq() << "\r\n";
    oss << "Session: " <<  randisss.str() << "\r\n";
    oss << "Transport: ";

    if (transport_mode_ == RTP_OVER_TCP) 
    {
        // RTP over TCP (interleaved方式)
        oss << "RTP/AVP/TCP;unicast;interleaved=" 
            << rtp_port << "-" << rtcp_port << ";mode=record" << "\r\n";\
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
    oss << "\r\n"; 
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

int RtspRequest::BuildANNOUNCERes(std::shared_ptr<char> data, int size)
{
    LOG_INFO("Building RTSP ANNOUNCE response with CSeq:" + this->GetCSeq());

    if (!data || size <= 0) return 0;
    //std::string body = sdp_->buildANNOUNCEBody();

    // 清空缓冲区（可选）
    memset(data.get(), 0, size);

    // snprintf 返回写入的字符数，不包括 '\0'
    // int written = snprintf(data.get(), size,
    //    "RTSP/1.0 200 OK\r\n"
    //     "CSeq: %s\r\n"
    //     "Content-Type: application/sdp\r\n"
    //     "Content-Length: %zu\r\n"
    //     "\r\n%s",
    //     this->GetCSeq().c_str(),
    //     body.size(),
    //     body.c_str()
    // );
    int written = snprintf(data.get(), size,
       "RTSP/1.0 200 OK\r\n"
        "CSeq: %s\r\n"
        "\r\n",
        this->GetCSeq().c_str()
    );


    if (written < 0 || written >= size) {
        LOG_ERROR("Failed to build RTSP ANNOUNCE response or buffer too small");
        return 0;
    }

    return written;  // 返回实际写入长度
}

/*
    RTSP/1.0 200 OK
    CSeq: 4
    Session: 6b8b4567
    Range: npt=0.000-
    Date: Tue, 14 Oct 2025 13:21:16 GMT
*/

int RtspRequest::BuildRecordRes(std::shared_ptr<char> data, int size,std::string session_id)
{
    LOG_INFO("Building RTSP RECORD response with CSeq:" + this->GetCSeq());
    if (!data || size <= 0) return 0;
    memset(data.get(), 0, size);
    snprintf(data.get(), size,
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %s\r\n"
        "Session: %s\r\n"
        "Range: npt=0.000-\r\n"
        "Date: %s\r\n"
        "\r\n",
        this->GetCSeq().c_str(),
        session_id.c_str(),
        this->GetGmtTimeString().c_str()
    );
    return (int)strlen(data.get());
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

    if(method_ == Method::SETUP)
    {
        size_t pos = uri_str.rfind('/');
        trackId = (pos != std::string::npos) ? uri_str.substr(pos + 1) : "";
        LOG_INFO("Extracted trackId: [" + trackId + "]");
    }

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
    if (line.find("CSeq:") == 0) 
    {
        if (!ParseCseq(line)) {
            LOG_ERROR("CSeq parsing failed");
            return false;
        }
        return true;
    }

    // 解析 Accept
    if (method_ == Method::DESCRIBE && line.find("Accept:") == 0) 
    {
        ParseAccept(line);
        return true;
    }

    // 解析 Transport
    if (method_ == Method::SETUP && line.find("Transport:") == 0) 
    {
        LOG_INFO("Parsing SETUP Transport header: " + line);
        if (!ParseTransport(line)) {
            LOG_ERROR("Failed to parse Transport header");
            return false;
        }
        return true;
    }

    // 解析 Session
    if ((method_ == Method::PLAY || method_ == Method::TEARDOWN || method_ == Method::SETUP) && line.find("Session:") == 0) 
    {
        if (!ParseSessionId(line)) {
            LOG_ERROR("Failed to parse Session header");
            return false;
        }
        return true;
    }

    // 解析 Authorization
    if (line.find("Authorization:") == 0) 
    {
        ParseAuthorization(line);
        return true;
    }

    //解析 Content-Length
    if (line.find("Content-Length:") == 0) 
    {
        int content_length = 0;
        if (sscanf(line.c_str(), "Content-Length: %d", &content_length) == 1) {
            content_length_ = std::to_string(content_length);
            header_line_param_["Content-Length"] = std::make_pair(content_length_, static_cast<uint32_t>(content_length));
            LOG_INFO("Parsed Content-Length: " + content_length_);
            return true;
        } 
        else 
        {
            LOG_ERROR("Failed to parse Content-Length value");
            return false;
        }
    }

    LOG_INFO("Unhandled or irrelevant header: " + line);
    return true;
}

bool RtspRequest::ParseBodyLine(const char *begin, const char *end)
{
    std::string body(begin, end);
    LOG_INFO("Parsing RTSP body line: [" + body + "]");
    std::istringstream iss(body);
    std::string line;
    sdp_ = std::make_shared<Sdp>();
    Sdp::MediaDescription media;

    bool inMedia = false;

    std::shared_ptr<Sdp::MediaDescription> currentMedia;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        LOG_INFO("Body line: [" + line + "]");

        if (line.rfind("m=", 0) == 0) {
            // 新建一个 media
            currentMedia = std::make_shared<Sdp::MediaDescription>();
            std::istringstream ms(line.substr(2));
            ms >> currentMedia->media_type >> currentMedia->port >> currentMedia->protocol >> currentMedia->payload_type;

            // 不 push_back，现在只创建
        }
        else if (line.rfind("a=rtpmap:", 0) == 0 && currentMedia) 
        {
            size_t sep = line.find(' ');
            if (sep != std::string::npos) {
                int pt = std::stoi(line.substr(9, sep - 9));
                std::string codec_info = line.substr(sep + 1);
                size_t slash = codec_info.find('/');
                if (slash != std::string::npos) {
                    currentMedia->codec_name = codec_info.substr(0, slash);
                    currentMedia->clock_rate = std::stoi(codec_info.substr(slash + 1));
                }
            }
        }
        else if (line.rfind("a=control:", 0) == 0 && currentMedia) 
        {
            currentMedia->control = line.substr(10);
            sdp_->media_list_.push_back(*currentMedia);  // 填充完成后 push_back
            currentMedia.reset();                        // 准备下一 media
        }
    }

    for (auto &m : sdp_->media_list_) {
        LOG_INFO("Media type: " + m.media_type + ", codec: " + m.codec_name +
                 ", payload: " + std::to_string(m.payload_type) +
                 ", clock: " + std::to_string(m.clock_rate) +
                 ", control: " + m.control);
    }

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
            LOG_INFO("Building RTSP ANNOUNCE response with CSeq:" + std::to_string(cseq));
            header_line_param_["CSeq"] = std::make_pair(std::to_string(cseq), 0);
            LOG_INFO("Building RTSP ANNOUNCE response with CSeq:" + this->GetCSeq());
            return true;
        }
    }
   
    return false;
}

bool RtspRequest::ParseSessionId(const std::string &line)
{
    std::size_t pos = line.find("Session:");
    if (pos != std::string::npos)
    {
        std::string session = line.substr(pos + 8);
        session.erase(0, session.find_first_not_of(" \t\r\n")); // 去
        session.erase(session.find_last_not_of(" \t\r\n") + 1); // 去除尾部空白
        session_id_ = static_cast<MediaChannelId>(std::stoi(session)); 
        header_line_param_["Session"] = std::make_pair(session_id_, 0);
        LOG_INFO("Parsed Session ID: " + session_id_);
        return true;
    }
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
