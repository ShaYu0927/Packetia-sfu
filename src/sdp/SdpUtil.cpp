#include "SdpUtil.h"
#include "SdpMode.h"
#include <vector>
#include <sstream>

namespace sdp
{

std::vector<std::string> SplitBySpace(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream iss(text);
    std::string token;
    while (iss >> token)
    {
        out.push_back(token);
    }
    return out;
}

std::string MakeLineError(const char* field, const SdpLine& line, const std::string& detail)
{
    std::string err = "parse ";
    err += field;
    err += " failed at line ";
    err += std::to_string(line.line_no);
    err += ": ";
    err += detail;
    err += ", raw=\"";
    err += line.raw;
    err += "\"";
    return err;
}


bool SdpParser::Parse(const std::string& text, SdpSession& session, std::string& err)
{
    err.clear();

    if(text.empty())
    {
        err = "sdp text is empty";
        return false;
    }

    SdpSession tmp_session;
    std::vector<SdpLine> lines = SplitLines(text);

    if (lines.empty())
    {
        err = "no valid sdp lines";
        return false;
    }

    if (!ParseLines(lines, tmp_session, err))
    {
        return false;
    }
    session = std::move(tmp_session);
    return true;
}

std::vector<SdpLine> SdpParser::SplitLines(const std::string& text)
{
    std::vector<SdpLine> lines;
    std::istringstream iss(text);
    std::string raw;
    size_t line_no = 0;

    while(std::getline(iss, raw))
    {
        ++line_no;
        if (!raw.empty() && raw.back() == '\r')
        {
            raw.pop_back();
        }

        if (raw.empty())
        {
            continue;
        }

        SdpLine line;
        line.line_no = line_no;
        line.value = raw;

        if(raw.size() >= 2  || raw[1] == '=')
        {
            line.type = raw[0];
            line.value = raw.substr(2);
        }
        else
        {
            line.type = 0;
            line.value = raw;
        }
        lines.emplace_back(std::move(line));
    }
    return lines;
}

    
bool SdpParser::ParseLines(const std::vector<SdpLine>& lines, SdpSession& session, std::string& err)
{
    SdpMedia* current_media = nullptr;

    for (const auto& line : lines)
    {
        switch(line.type)
        {
        case 'v':
            if (!ParseVersion(line, session, err))
                return false;
            break;

        case 'o':
            if (!ParseOrigin(line, session, err))
                return false;
            break;

        case 's':
            if (!ParseSessionName(line, session, err))
                return false;
            break;

        case 'c':
            if (!ParseConnection(line, session, current_media, err))
                return false;
            break;

        case 't':
            if (!ParseTiming(line, session, err))
                return false;
            break;

        case 'm':
            if (!ParseMedia(line, session, current_media, err))
                return false;
            break;

        case 'a':
            if (!ParseAttribute(line, session, current_media, err))
                return false;
            break;

        }
    }

    return true;
}

bool SdpParser::ParseVersion(const SdpLine& line, SdpSession& session, std::string& err)
{
     if (line.value.empty())
    {
        err = MakeLineError("v=", line, "empty version");
        return false;
    }

    try
    {
        int ver = std::stoi(line.value);
        if (ver != 0)
        {
            err = MakeLineError("v=", line, "unsupported version: " + line.value);
            return false;
        }

        session.version = ver;
        return true;
    }
    catch (const std::exception&)
    {
        err = MakeLineError("v=", line, "invalid integer version: " + line.value);
        return false;
    }
}

bool SdpParser::ParseOrigin(const SdpLine& line, SdpSession& session, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("o=", line, "empty origin");
        return false;
    }

    std::vector<std::string> parts = SplitBySpace(line.value);
    if (parts.size() != 6)
    {
        err = MakeLineError("o=", line, "origin must have 6 fields");
        return false;
    }

    session.origin.username        = parts[0];
    session.origin.sess_id         = parts[1];
    session.origin.sess_version    = parts[2];
    session.origin.net_type        = parts[3];
    session.origin.addr_type       = parts[4];
    session.origin.unicast_address = parts[5];

    return true;
}


bool SdpParser::ParseSessionName(const SdpLine& line, SdpSession& session, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("s=", line, "empty session name");
        return false;
    }

    session.session_name = line.value;
    return true;
}

bool SdpParser::ParseConnection(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("c=", line, "empty connection");
        return false;
    }

    std::vector<std::string> parts = SplitBySpace(line.value);
    if (parts.size() != 3)
    {
        err = MakeLineError("c=", line, "connection must have 3 fields");
        return false;
    }

    SdpConnection conn;
    conn.net_type  = parts[0];
    conn.addr_type = parts[1];
    conn.address   = parts[2];

    if (current_media)
    {
        current_media->conn = std::move(conn);
    }
    else
    {
        session.conn = std::move(conn);
    }

    return true;
}

bool SdpParser::ParseTiming(const SdpLine& line, SdpSession& session, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("t=", line, "empty timing");
        return false;
    }

    std::vector<std::string> parts = SplitBySpace(line.value);
    if (parts.size() != 2)
    {
        err = MakeLineError("t=", line, "timing must have 2 fields");
        return false;
    }

    session.timing = line.value;
    return true;
}

bool SdpParser::ParseMedia(const SdpLine& line, SdpSession& session, SdpMedia*& current_media, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("m=", line, "empty media");
        return false;
    }

    std::vector<std::string> parts = SplitBySpace(line.value);
    if (parts.size() < 4)
    {
        err = MakeLineError("m=", line, "media must have at least 4 fields");
        return false;
    }

    SdpMedia media;
    media.media = parts[0];
    media.proto = parts[2];

    try
    {
        media.port = std::stoi(parts[1]);
    }
    catch (const std::exception&)
    {
        err = MakeLineError("m=", line, "invalid media port: " + parts[1]);
        return false;
    }

    for (size_t i = 3; i < parts.size(); ++i)
    {
        media.fmts.emplace_back(parts[i]);
    }

    session.medias.push_back(std::move(media));
    current_media = &session.medias.back();

    return true;
}

bool SdpParser::ParseAttribute(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err)
{
    if (line.value.empty())
    {
        err = MakeLineError("a=", line, "empty attribute");
        return false;
    }

    SdpAttribute attr = SplitAttribute(line.value);
    if (attr.key.empty())
    {
        err = MakeLineError("a=", line, "invalid attribute key");
        return false;
    }

    if (current_media)
    {
        current_media->attributes.push_back(std::move(attr));
    }
    else
    {
        session.attributes.push_back(std::move(attr));
    }

    return true;
}

SdpAttribute SdpParser::SplitAttribute(const std::string& text)
{
    SdpAttribute attr;

    auto pos = text.find(':');
    if (pos == std::string::npos)
    {
        attr.key = text;
        attr.value.clear();
        return attr;
    }

    attr.key = text.substr(0, pos);
    attr.value = text.substr(pos + 1);
    return attr;
}

}