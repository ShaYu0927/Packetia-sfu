#include "SdpUtil.h"

#include <cstdlib>
#include <sstream>

namespace sdp 
{

static std::vector<std::string> SplitBySpace(const std::string& s)
{
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string token;
    while (iss >> token)
    {
        out.push_back(token);
    }
    return out;
}

bool SdpParser::Parse(const std::string& text, SdpSession& session, std::string& err)
{
    std::vector<SdpLine> lines;

    return ParseLines(lines, session, err);
}

bool SdpParser::ParseLines(const std::vector<SdpLine>& lines, SdpSession& session, std::string& err)
{
    SdpMedia* current_media = nullptr;

    for (const auto& line : lines)
    {
        switch (line.type)
        {
        case 'v':
            if (!ParseVersion(line, session, err)) return false;
            break;
        case 'o':
            if (!ParseOrigin(line, session, err)) return false;
            break;
        case 's':
            if (!ParseSessionName(line, session, err)) return false;
            break;
        case 'c':
            if (!ParseConnection(line, session, current_media, err)) return false;
            break;
        case 't':
            if (!ParseTiming(line, session, err)) return false;
            break;
        case 'm':
            if (!ParseMedia(line, session, current_media, err)) return false;
            break;
        case 'a':
            if (!ParseAttribute(line, session, current_media, err)) return false;
            break;
        default:
            // 先忽略未知字段，也可以改成报错
            break;
        }
    }

    return true;
}

bool SdpParser::ParseVersion(const SdpLine& line, SdpSession& session, std::string& err)
{
    try
    {
        session.version = std::stoi(line.value);
        return true;
    }
    catch (...)
    {
        err = "invalid version at line " + std::to_string(line.line_no);
        return false;
    }
}

bool SdpParser::ParseOrigin(const SdpLine& line, SdpSession& session, std::string& err)
{
    (void)err;
    session.origin = line.value;
    return true;
}

bool SdpParser::ParseSessionName(const SdpLine& line, SdpSession& session, std::string& err)
{
    (void)err;
    session.session_name = line.value;
    return true;
}

bool SdpParser::ParseConnection(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err)
{
    (void)err;
    if (current_media)
        current_media->connection = line.value;
    else
        session.connection = line.value;
    return true;
}

bool SdpParser::ParseTiming(const SdpLine& line, SdpSession& session, std::string& err)
{
    (void)err;
    session.timing = line.value;
    return true;
}

bool SdpParser::ParseMedia(const SdpLine& line, SdpSession& session, SdpMedia*& current_media, std::string& err)
{
    auto parts = SplitBySpace(line.value);
    if (parts.size() < 4)
    {
        err = "invalid media description at line " + std::to_string(line.line_no);
        return false;
    }

    SdpMedia media;
    media.media = parts[0];

    try
    {
        media.port = std::stoi(parts[1]);
    }
    catch (...)
    {
        err = "invalid media port at line " + std::to_string(line.line_no);
        return false;
    }

    media.proto = parts[2];

    for (size_t i = 3; i < parts.size(); ++i)
    {
        try
        {
            media.fmts.push_back(std::stoi(parts[i]));
        }
        catch (...)
        {
        }
    }

    session.medias.push_back(std::move(media));
    current_media = &session.medias.back();
    return true;
}

SdpAttribute SdpParser::SplitAttribute(const std::string& text)
{
    auto pos = text.find(':');
    if (pos == std::string::npos)
    {
        return {text, ""};
    }

    return {text.substr(0, pos), text.substr(pos + 1)};
}

bool SdpParser::ParseAttribute(const SdpLine& line, SdpSession& session, SdpMedia* current_media, std::string& err)
{
    (void)err;
    SdpAttribute attr = SplitAttribute(line.value);

    if (current_media)
        current_media->attributes.push_back(std::move(attr));
    else
        session.attributes.push_back(std::move(attr));

    return true;
}

} // namespace sdp