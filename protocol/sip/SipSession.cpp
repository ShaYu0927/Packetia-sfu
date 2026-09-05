#include "SipSession.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace sip
{
namespace
{
std::string ToLower(std::string s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void Trim(std::string& s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.pop_back();
}

bool SplitOnce(const std::string& s, char delim, std::string& left, std::string& right)
{
    size_t pos = s.find(delim);
    if (pos == std::string::npos) return false;
    left = s.substr(0, pos);
    right = s.substr(pos + 1);
    return true;
}

int FindCrlfCrlf(const char* p, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i)
    {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ParseContentLengthFromHeader(const char* header, size_t header_len, size_t& out_len)
{
    out_len = 0;
    size_t i = 0;
    while (i < header_len)
    {
        size_t line_end = i;
        while (line_end + 1 < header_len && !(header[line_end] == '\r' && header[line_end + 1] == '\n'))
        {
            ++line_end;
        }

        size_t line_len = line_end > i ? line_end - i : 0;
        if (line_len == 0) break;

        std::string line(header + i, line_len);
        std::string key;
        std::string value;
        if (SplitOnce(line, ':', key, value))
        {
            Trim(key);
            Trim(value);
            if (ToLower(key) == "content-length")
            {
                char* end = nullptr;
                unsigned long len = std::strtoul(value.c_str(), &end, 10);
                if (end == value.c_str()) return false;
                out_len = static_cast<size_t>(len);
                return true;
            }
        }

        i = line_end + 2;
    }
    return true;
}

std::vector<std::string> SplitLines(const std::string& header)
{
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= header.size())
    {
        size_t eol = header.find("\r\n", pos);
        if (eol == std::string::npos)
        {
            lines.push_back(header.substr(pos));
            break;
        }
        lines.push_back(header.substr(pos, eol - pos));
        pos = eol + 2;
    }
    return lines;
}

std::string HeaderOne(const SipHeaders& headers, const std::string& name)
{
    return headers.get_one(name);
}

std::string EnsureToTag(std::string to)
{
    if (ToLower(to).find(";tag=") != std::string::npos) return to;
    return to + ";tag=packetia-sip";
}
} // namespace

SipSession::SipSession(TcpConnection::Ptr conn)
    : conn_(std::move(conn))
{
}

SipSession::~SipSession()
{
    LOG_INFO("SipSession destroyed, fd=", conn_ ? conn_->GetSocket() : -1);
}

void SipSession::Start()
{
    LOG_INFO("SipSession started, fd=", conn_ ? conn_->GetSocket() : -1);
}

void SipSession::OnClosed(int reason)
{
    LOG_INFO("SipSession closed, fd=", conn_ ? conn_->GetSocket() : -1, " reason=", reason);
}

bool SipSession::OnRead(TcpConnection::Ptr conn, BufferReader& buffer)
{
    (void)conn;

    constexpr size_t kByteBudget = 256 * 1024;
    constexpr size_t kFrameBudget = 512;
    size_t processed_bytes = 0;
    size_t processed_frames = 0;

    while (buffer.ReadableBytes() > 0)
    {
        if (processed_bytes >= kByteBudget || processed_frames >= kFrameBudget) break;

        size_t before = buffer.ReadableBytes();
        auto result = TryConsumeOne(buffer);
        if (result == ConsumeResult::NeedMore) break;
        if (result == ConsumeResult::Error) return false;

        size_t after = buffer.ReadableBytes();
        if (after >= before) return false;
        processed_bytes += before - after;
        ++processed_frames;
    }

    return true;
}

SipSession::ConsumeResult SipSession::TryConsumeOne(BufferReader& buffer)
{
    size_t n = buffer.ReadableBytes();
    if (n == 0) return ConsumeResult::NeedMore;

    const char* p = buffer.Peek();
    int header_end = FindCrlfCrlf(p, n);
    if (header_end < 0)
    {
        constexpr size_t kMaxHeader = 32 * 1024;
        return n > kMaxHeader ? ConsumeResult::Error : ConsumeResult::NeedMore;
    }

    size_t header_len = static_cast<size_t>(header_end) + 4;
    size_t body_len = 0;
    if (!ParseContentLengthFromHeader(p, header_len, body_len)) return ConsumeResult::Error;

    constexpr size_t kMaxBody = 2 * 1024 * 1024;
    if (body_len > kMaxBody) return ConsumeResult::Error;

    size_t total = header_len + body_len;
    if (n < total) return ConsumeResult::NeedMore;

    SipMessage msg;
    if (!ParseMessage(p, total, msg)) return ConsumeResult::Error;

    Dispatch(msg);
    buffer.Retrieve(total);
    return ConsumeResult::Consumed;
}

bool SipSession::ParseMessage(const char* data, size_t len, SipMessage& out)
{
    std::string raw(data, len);
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    std::string header = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);
    auto lines = SplitLines(header);
    if (lines.empty() || lines.front().empty()) return false;

    const std::string& start = lines.front();
    if (start.find("SIP/2.0") == 0)
    {
        std::istringstream iss(start);
        SipResponse resp;
        iss >> resp.version >> resp.status_code;
        std::getline(iss, resp.reason_phrase);
        Trim(resp.reason_phrase);
        resp.body = std::move(body);
        out.type = SipMsgType::Response;
        out.msg = std::move(resp);
        return true;
    }

    std::istringstream iss(start);
    SipRequest req;
    iss >> req.method >> req.uri >> req.version;
    if (req.method.empty() || req.uri.empty() || req.version != "SIP/2.0") return false;

    for (size_t i = 1; i < lines.size(); ++i)
    {
        if (lines[i].empty()) continue;
        std::string key;
        std::string value;
        if (!SplitOnce(lines[i], ':', key, value)) continue;
        Trim(key);
        Trim(value);
        req.headers.add(std::move(key), std::move(value));
    }
    req.headers.build_index();
    req.body = std::move(body);

    out.type = SipMsgType::Request;
    out.msg = std::move(req);
    return true;
}

void SipSession::Dispatch(const SipMessage& msg)
{
    if (!msg.is_request())
    {
        const SipResponse& resp = std::get<SipResponse>(msg.msg);
        LOG_INFO("SIP response received, status=", resp.status_code, " reason=", resp.reason_phrase);
        return;
    }

    const SipRequest& req = std::get<SipRequest>(msg.msg);
    LOG_INFO("SIP request received, method=", req.method, " uri=", req.uri);

    if (req.method == "ACK")
    {
        return;
    }

    if (req.method == "OPTIONS")
    {
        SendResponse(req, 200, "OK", "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, REGISTER, MESSAGE\r\nAccept: application/sdp\r\n");
        return;
    }

    if (req.method == "REGISTER" || req.method == "BYE" || req.method == "CANCEL" || req.method == "MESSAGE")
    {
        SendResponse(req, 200, "OK");
        return;
    }

    if (req.method == "INVITE")
    {
        SendResponse(req, 100, "Trying");
        SendResponse(req, 501, "Not Implemented", "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, REGISTER, MESSAGE\r\n");
        return;
    }

    SendResponse(req, 501, "Not Implemented", "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, REGISTER, MESSAGE\r\n");
}

void SipSession::SendResponse(const SipRequest& req, int status_code, const std::string& reason, const std::string& extra_headers, const std::string& body)
{
    std::ostringstream oss;
    oss << "SIP/2.0 " << status_code << ' ' << reason << "\r\n";

    for (const auto& via : req.headers.get_all("Via"))
    {
        oss << "Via: " << via << "\r\n";
    }

    std::string from = HeaderOne(req.headers, "From");
    std::string to = HeaderOne(req.headers, "To");
    std::string call_id = HeaderOne(req.headers, "Call-ID");
    std::string cseq = HeaderOne(req.headers, "CSeq");

    if (!from.empty()) oss << "From: " << from << "\r\n";
    if (!to.empty()) oss << "To: " << EnsureToTag(to) << "\r\n";
    if (!call_id.empty()) oss << "Call-ID: " << call_id << "\r\n";
    if (!cseq.empty()) oss << "CSeq: " << cseq << "\r\n";

    oss << "Server: Packetia SIP Server\r\n";
    oss << extra_headers;
    oss << "Content-Length: " << body.size() << "\r\n";
    if (!body.empty()) oss << "Content-Type: application/sdp\r\n";
    oss << "\r\n";
    oss << body;

    SendRaw(oss.str());
}

void SipSession::SendRaw(const std::string& data)
{
    if (!conn_ || data.empty()) return;
    if (conn_->Send(data.data(), static_cast<uint32_t>(data.size())) != TcpConnection::SendResult::Queued)
    {
        LOG_ERROR("SIP response could not be queued; closing connection");
        conn_->Disconnect();
    }
}

} // namespace sip
