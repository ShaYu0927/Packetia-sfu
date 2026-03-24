#include "RtspMessage.h"
#include "MediaSession.h"
#include <string>
#include "Sdp.h"
#include "RtspUtil.h"
#include "logger.h"


static inline void Trim(std::string& s)
{
    auto not_space = [](unsigned char c){ return !std::isspace(c); };

    while (!s.empty() && !not_space((unsigned char)s.front()))
        s.erase(s.begin());

    while (!s.empty() && !not_space((unsigned char)s.back()))
        s.pop_back();
}

static inline bool SplitOnce(const std::string& s, char delim, std::string& left, std::string& right) 
{
    auto pos = s.find(delim);
    if (pos == std::string::npos) return false;
    left = s.substr(0, pos);
    right = s.substr(pos + 1);
    return true;
}

static inline std::string ToLower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}


bool RtspRequest::ParseRequest(const char* p, size_t total, RtspRequestInfo& out)
{
    std::string msg(p, total);
    auto hdrEnd = msg.find("\r\n\r\n");

    if (hdrEnd == std::string::npos) return false;

    std::string header = msg.substr(0, hdrEnd);
    out.body = msg.substr(hdrEnd + 4);

    size_t pos = 0;
    auto nextLine = [&](std::string& line)->bool{
        if (pos > header.size()) return false;
        size_t eol = header.find("\r\n", pos);
        if (eol == std::string::npos) {
            line = header.substr(pos);
            pos = header.size() + 1;
            return true;
        }
        line = header.substr(pos, eol - pos);
        pos = eol + 2;
        return true;
    };

    std::string line;
    if (!nextLine(line)) return false;
    {
        size_t a = line.find(' ');
        if (a == std::string::npos) return false;
        size_t b = line.find(' ', a + 1);
        if (b == std::string::npos) return false;

        out.method  = line.substr(0, a);
        out.url     = line.substr(a + 1, b - (a + 1));
        out.version = line.substr(b + 1);
        Trim(out.method); Trim(out.url); Trim(out.version);
        if (out.method.empty() || out.url.empty()) return false;
    }

    while (pos <= header.size())
    {
        if (!nextLine(line)) break;
        if (line.empty()) break;

        std::string k, v;
        if (!SplitOnce(line, ':', k, v))
        {
            continue;
        }

        Trim(k); Trim(v);
        k = ToLower(k);
        out.headers[k] = v;
        if (k == "cseq") 
        {
            out.cseq = std::stoi(v);
        }
    }

    

    return true;
}


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



int RtspRequest::BuildOptionsRes(const RtspRequestInfo& req,
                                 std::shared_ptr<char> data,
                                 int size)
{
    if (!data || size <= 0)
    {
        return -1;
    }

    memset(data.get(), 0, size);

    LOG_DEBUG("Building RTSP OPTIONS response with cseq=" + std::to_string(req.cseq));

    int ret = snprintf(data.get(), size,
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n"
        "Server: MyRtspServer\r\n"
        "\r\n",
        req.cseq);

    if (ret < 0 || ret >= size)
    {
        return -1;
    }

    return ret;
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
			"CSeq: %s\r\n"
			"\r\n",
			this->GetCSeq().c_str());

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

int RtspRequest::BuildANNOUNCERes(const RtspRequestInfo& req,std::shared_ptr<char> data, int size)
{
    if (!data || size <= 0) return 0;
    memset(data.get(), 0, size);
    int written = snprintf(data.get(), size,
       "RTSP/1.0 200 OK\r\n"
        "CSeq: %s\r\n"
        "\r\n",
        std::to_string(req.cseq).c_str()
    );

    if (written < 0 || written >= size) 
    {
        LOG_ERROR("Failed to build RTSP ANNOUNCE response or buffer too small");
        return 0;
    }

    return written;
}

int RtspRequest::BuildRecordRes(const RtspRequestInfo& req,std::shared_ptr<char> data, int size)
{
    if (!data || size <= 0)
        return 0;

    memset(data.get(), 0, size);
    std::string cseq = std::to_string(req.cseq);
    std::string session = req.GetHeader("Session");
    std::string timestamp = std::to_string(Timestamp::NowMs());

    int ret = snprintf(
        data.get(), size,
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %s\r\n"
        "Session: %s\r\n"
        "Range: npt=0.000-\r\n"
        "Date: %s\r\n"
        "\r\n",
        cseq.c_str(),
        session.c_str(),
        timestamp.c_str()
    );

    if (ret < 0)
        return 0;

    return ret;
}

std::string RtspRequest::BuildSetupRes(const std::string& cseq,
                          const std::string& session_id,
                          int rtp_channel,
                          int rtcp_channel,
                          const std::string& mode)
{
    std::ostringstream oss;

    oss << "RTSP/1.0 200 OK\r\n";
    oss << "CSeq: " << cseq << "\r\n";
    oss << "Session: " << session_id << ";timeout=60\r\n";
    oss << "Transport: RTP/AVP/TCP;unicast;interleaved="
        << rtp_channel << "-" << rtcp_channel << "\r\n";
    oss << "\r\n";

    return oss.str();
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

    uint16_t port = 0;
    char ip[64] = {0};
    char suffix[64] = {0};
    bool parse_ok = false;

    if (sscanf(uri_str.c_str(), "%63[^:]:%hu/%63s", ip, &port, suffix) == 3) 
    {
        LOG_INFO("Parsed uri with port and suffix: ip=[" + std::string(ip) + "], port=[" + std::to_string(port) + "], suffix=[" + std::string(suffix) + "]");
        parse_ok = true;
    } 
    else if (sscanf(uri_str.c_str(), "%63[^:]:%hu", ip, &port) == 2) 
    {
        LOG_INFO("Parsed uri with port only: ip=[" + std::string(ip) + "], port=[" + std::to_string(port) + "]");
        suffix[0] = '\0';
        parse_ok = true;
    } 
    else if (sscanf(uri_str.c_str(), "%63[^/]/%63s", ip, suffix) == 2) 
    {
        port = 554;
        LOG_INFO("Parsed uri without port: ip=[" + std::string(ip) + "], suffix=[" + std::string(suffix) + "]");
        parse_ok = true;
    } 
    else if (sscanf(uri_str.c_str(), "%63s", ip) == 1) 
    {
        port = 554;
        suffix[0] = '\0';
        LOG_INFO("Parsed uri with only ip: ip=[" + std::string(ip) + "]");
        parse_ok = true;
    }

    if (!parse_ok) 
    {
        LOG_ERROR("Failed to parse ip/port/suffix from uri: [" + uri_str + "]");
        return false;
    }

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

#if RTP_DEBUG
            LOG_INFO("Parsing RTSP header line: [" + line + "]");
#endif
    

    if (line.empty()) 
    {
        LOG_INFO("Empty header line encountered");
        return false;
    }

    if (line.find("CSeq:") == 0) 
    {
        if (!ParseCseq(line)) {
            LOG_ERROR("CSeq parsing failed");
            return false;
        }
        return true;
    }

    if (method_ == Method::DESCRIBE && line.find("Accept:") == 0) 
    {
        ParseAccept(line);
        return true;
    }

    if (method_ == Method::SETUP && line.find("Transport:") == 0) 
    {
        LOG_INFO("Parsing SETUP Transport header: " + line);
        if (!ParseTransport(line)) {
            LOG_ERROR("Failed to parse Transport header");
            return false;
        }
        return true;
    }

    if ((method_ == Method::PLAY || method_ == Method::TEARDOWN || method_ == Method::SETUP) && line.find("Session:") == 0) 
    {
        if (!ParseSessionId(line)) {
            LOG_ERROR("Failed to parse Session header");
            return false;
        }
        return true;
    }

    if (line.find("Authorization:") == 0) 
    {
        ParseAuthorization(line);
        return true;
    }

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
        LOG_INFO("Parsed Session ID: " + std::to_string(session_id_));
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
    while (std::getline(iss, line)) 
    {
        // 移除行尾 \r
        if (!line.empty() && line.back() == '\r') 
        {
            line.pop_back();
        }

        if (line.find("Transport:") != std::string::npos) 
        {
            LOG_INFO("Parsing Transport line: " + line);

            // 判断 TCP
            if (line.find("interleaved=") != std::string::npos) 
            {
                // RTP over TCP
                size_t pos = line.find("interleaved=");
                std::string channels = line.substr(pos + strlen("interleaved="));
                auto dash = channels.find('-');
                if (dash != std::string::npos) 
                {
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
            else if (line.find("client_port=") != std::string::npos) 
            {
                size_t port_pos = line.find("client_port=");
                std::string ports = line.substr(port_pos + strlen("client_port="));
                auto dash = ports.find('-');
                if (dash != std::string::npos) 
                {
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
	if(iter != request_line_param_.end()) 
    {
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

std::string RtspRequest::HandleCmdOptions(RtspRequestInfo& req)
{
    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = BuildOptionsRes(req,res, 1024);
    return res.get();
}

std::string RtspRequest::HandleCmdDescribe(RtspRequestInfo& req)
{
    std::string message;
    if (req.cseq < 0) return message;
    if (req.GetHeader("Content-Type") != "application/sdp") return message;
    if (req.body.empty()) return message;

    // std::string streamName = ParseStreamName(req.url);

    std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
    int MessageSize = BuildANNOUNCERes(req,res, 4096);
    return res.get();
}

std::string RtspRequest::HandleCmdANNOUNCE(RtspRequestInfo& req)
{
    std::string err, suffix;

    std::string contentType = req.GetHeader("Content-Type");
    if (req.body.empty())
    {
        LOG_INFO("ANNOUNCE SDP body:", req.body.c_str());
    }

    suffix = rtsp::RtspUtil::GetSuffixFromSetupUrl(req.url);

    /* handle sdp */
    auto result = sdp::Sdp::Parse(req.body);
    if (!result.ok)
    {
        LOG_ERROR("Parse SDP failed, line=%zu, msg=%s",
                  result.line, result.message.c_str());
        return "";
    }

    auto session = MediaSessionManager::Instance().GetSessionBySuffix(suffix);
    if (!session)
    {
        session = MediaSession::CreateNew(suffix);
        MediaSessionManager::Instance().AddSession(session, suffix);
    }

    if (!session->ApplySdp(result.session, &err))
    {
        LOG_ERROR("ApplySdp failed:", err.c_str());
        return "";
    }

    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = BuildANNOUNCERes(req,res,1024);
    return res.get();
}

std::string RtspRequest::HandleCmdSetup(RtspRequestInfo& req)
{
    std::string session_id,url,control, suffix;
    std::shared_ptr<RtpTrack> track_ptr;
    std::shared_ptr<char> response(new char[10240], std::default_delete<char[]>());
    size_t size,transport = 1;
    uint16_t rtp_ch, rtcp_ch = 0;
    RtspTransport pTranOut;
    int ret;

    LOG_INFO(req.url);
    suffix = rtsp::RtspUtil::GetSuffixFromSetupUrl(req.url);
    control = req.GetControlFromUrl();

    LOG_INFO(control);

    media_session = MediaSessionManager::Instance().GetSessionBySuffix(suffix);
    if (!media_session)
    {
        LOG_ERROR("Media session not found, url=", suffix);
        return "";
    }

    session_id = std::to_string(media_session->GetId());
    auto tracker = media_session->GetRtpTrack(control);
    if(!tracker)
    {
        LOG_ERROR("Track not found, suffix=, control=", suffix, control);
        return "";
    }

    std::string transport_str = req.GetHeader("transport");
    if (transport_str.empty())
    {
        LOG_ERROR("SETUP missing Transport header");
        return "";
    }
    ret = rtsp::RtspUtil::ParseTransport(transport_str, pTranOut);
    if(!ret)
    {
        LOG_ERROR("SETUP missing Transport header");
        return "";
    }

    if (pTranOut.interleaved_rtp < 0 || pTranOut.interleaved_rtcp < 0)
    {
        LOG_ERROR("SETUP missing interleaved channel, transport={}", transport_str);
        return "";
    }

    tracker->setInterleavedChannel(pTranOut.interleaved_rtp, pTranOut.interleaved_rtcp);

    std::string str = BuildSetupRes(std::to_string(req.cseq), session_id,pTranOut.interleaved_rtp, pTranOut.interleaved_rtcp,"record");
    LOG_INFO(str);
    
    return str;
}

std::string RtspRequest::HandleCmdRecord(RtspRequestInfo& req)
{
    std::string cseq = req.GetHeader("CSeq");
    std::string session_id = req.GetHeader("session");

    if (session_id.empty())
    {
        return "";
    }

    auto session = MediaSessionManager::Instance().GetSessionById(std::stoi(session_id));
    if (!session)
    {
        return "";
    }

    std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
    int size = BuildRecordRes(req, res, 1024);
    return res.get();
}

std::string RtspRequest::HandleCmdPlay(RtspRequestInfo& req)
{

}

std::string RtspRequest::HandleCmdPause(RtspRequestInfo& req)
{

}

std::string RtspRequest::HandleCmdTeardown(RtspRequestInfo& req)
{

}
