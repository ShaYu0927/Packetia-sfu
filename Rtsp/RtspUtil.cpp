#include "RtspUtil.h"
#include "logger.h"
#include <cstring>

namespace rtsp
{

static std::vector<std::string> Split(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::string cur;

    for (char ch : s)
    {
        if (ch == delim)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(ch);
        }
    }

    out.push_back(cur);
    return out;
}

static std::string Trim(const std::string& s)
{
    size_t begin = 0;
    size_t end = s.size();

    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
    {
        ++begin;
    }

    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }

    return s.substr(begin, end - begin);
}


std::optional<int> RtspUtil::ParseStreamId(const std::string &control)
{
    static const std::string kKey = "streamid=";

    auto pos = control.find(kKey);
    if (pos == std::string::npos) 
    {
        LOG_ERROR("ParseStreamId: no 'streamid=' in control=", control);
        return -1;
    }

    std::string num = control.substr(pos + kKey.size());
    if (num.empty()) 
    {
        LOG_ERROR("ParseStreamId: empty streamid in control=", control);
        return -1;
    }

    try 
    {
        int idx = std::stoi(num);
        if (idx < 0) 
        {
            LOG_ERROR("ParseStreamId: negative streamid=", idx);
            return -1;
        }
        return idx;
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR("ParseStreamId: invalid streamid=", num, " err=", e.what());
        return -1;
    }
}


std::string RtspUtil::GetSuffixFromSetupUrl(const std::string& url)
{
    std::string s = url;

    auto pos = s.find("://");
    if (pos != std::string::npos)
    {
        auto path_pos = s.find('/', pos + 3);
        if (path_pos != std::string::npos)
            s = s.substr(path_pos); 
        else
            return "";
    }

    auto last_slash = s.rfind('/');
    if (last_slash == std::string::npos)
    {
        if (!s.empty() && s[0] == '/')
            return s.substr(1);
        return s;
    }

    std::string tail = s.substr(last_slash + 1);
    if (tail.find("streamid=") == 0 || tail.find("trackID=") == 0)
        s = s.substr(0, last_slash);

    if (!s.empty() && s[0] == '/')
        s.erase(0, 1);

    return s;
}

bool RtspUtil::ParseTransport(const std::string& text, RtspTransport& out)
{
    auto parts = Split(text, ';');
    if (parts.empty())
        return false;

    out.profile = Trim(parts[0]);

    if (out.profile.find("/TCP") != std::string::npos)
        out.lower_transport = "TCP";
    else
        out.lower_transport = "UDP";

    for (size_t i = 1; i < parts.size(); ++i)
    {
        std::string item = Trim(parts[i]);

        if (item == "unicast")
        {
            out.unicast = true;
            out.multicast = false;
        }
        else if (item == "multicast")
        {
            out.unicast = false;
            out.multicast = true;
        }
        else if (item.rfind("interleaved=", 0) == 0)
        {
            std::string v = item.substr(strlen("interleaved="));
            auto pos = v.find('-');
            if (pos == std::string::npos)
                return false;

            out.interleaved_rtp = std::atoi(v.substr(0, pos).c_str());
            out.interleaved_rtcp = std::atoi(v.substr(pos + 1).c_str());
        }
        else if (item.rfind("client_port=", 0) == 0)
        {
            std::string v = item.substr(strlen("client_port="));
            auto pos = v.find('-');
            if (pos == std::string::npos)
                return false;

            out.client_rtp_port = std::atoi(v.substr(0, pos).c_str());
            out.client_rtcp_port = std::atoi(v.substr(pos + 1).c_str());
        }
        else if (item.rfind("server_port=", 0) == 0)
        {
            std::string v = item.substr(strlen("server_port="));
            auto pos = v.find('-');
            if (pos == std::string::npos)
                return false;

            out.server_rtp_port = std::atoi(v.substr(0, pos).c_str());
            out.server_rtcp_port = std::atoi(v.substr(pos + 1).c_str());
        }
        else if (item.rfind("mode=", 0) == 0)
        {
            std::string mode = item.substr(std::string("mode=").size());
            mode = Trim(mode);

            if (!mode.empty() && mode.front() == '"')
                mode.erase(mode.begin());
            if (!mode.empty() && mode.back() == '"')
                mode.pop_back();

            if (mode == "record")
            {
                out.mode = RtspMode::RECORD;
            }
            else if (mode == "play")
            {
                out.mode = RtspMode::PLAY;
            }
            else
            {
                out.mode = RtspMode::ModeUnknown;
            }
        }
    }

    return true;
}

bool RtspUtil::ParseRtpMapLine(const std::string& line,
                            int* payload_type,
                            std::string* codec_name,
                            uint32_t* clock_rate,
                            int* channels)
{
    // line examples:
    // "96 H264/90000"
    // "97 MPEG4-GENERIC/44100/2"

    if (!payload_type || !codec_name || !clock_rate || !channels)
    {
        return false;
    }

    *payload_type = -1;
    codec_name->clear();
    *clock_rate = 0;
    *channels = 0;

    auto sp = line.find(' ');
    if (sp == std::string::npos)
    {
        return false;
    }

    std::string pt_str = line.substr(0, sp);
    std::string enc = line.substr(sp + 1);

    try
    {
        *payload_type = std::stoi(pt_str);
    }
    catch (...)
    {
        return false;
    }

    // enc examples:
    // H264/90000
    // MPEG4-GENERIC/44100/2
    size_t p1 = enc.find('/');
    if (p1 == std::string::npos)
    {
        return false;
    }

    *codec_name = enc.substr(0, p1);

    size_t p2 = enc.find('/', p1 + 1);
    try
    {
        if (p2 == std::string::npos)
        {
            *clock_rate = static_cast<uint32_t>(std::stoul(enc.substr(p1 + 1)));
            *channels = 0;
        }
        else
        {
            *clock_rate = static_cast<uint32_t>(std::stoul(enc.substr(p1 + 1, p2 - p1 - 1)));
            *channels = std::stoi(enc.substr(p2 + 1));
        }
    }
    catch (...)
    {
        return false;
    }

    return true;
}

std::string RtspUtil::StripFmtpPayloadPrefix(const std::string& fmtp_line)
{
    // "96 packetization-mode=1; profile-level-id=..."
    // -> "packetization-mode=1; profile-level-id=..."

    auto sp = fmtp_line.find(' ');
    if (sp == std::string::npos)
    {
        return fmtp_line;
    }
    return fmtp_line.substr(sp + 1);
}

}