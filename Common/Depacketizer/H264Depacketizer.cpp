#include "H264Depacketizer.h"
#include <sstream>
#include <algorithm>
#include <cctype>

H264Depacketizer::H264Depacketizer(const std::string& fmtp)
{
    auto trim = [](const std::string& text) {
        const auto begin = text.find_first_not_of(" \t\r\n");
        return begin == std::string::npos ? std::string{} :
            text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    };
    std::istringstream parameters(fmtp);
    std::string parameter;
    while (std::getline(parameters, parameter, ';')) {
        const auto equal = parameter.find('=');
        if (equal == std::string::npos) continue;
        auto key = trim(parameter.substr(0, equal));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (key != "sprop-parameter-sets") continue;
        std::istringstream values(parameter.substr(equal + 1));
        std::string encoded;
        while (std::getline(values, encoded, ',')) {
            encoded = trim(encoded);
            if (encoded.empty() || encoded.size() > 16384) continue;
            std::vector<uint8_t> decoded;
            uint32_t accumulator = 0;
            int bits = 0;
            bool valid = true, padding = false;
            for (unsigned char c : encoded) {
                if (c == '=') { padding = true; continue; }
                const int digit = c >= 'A' && c <= 'Z' ? c - 'A' :
                    c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                    c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
                if (digit < 0 || padding) { valid = false; break; }
                accumulator = (accumulator << 6) | digit;
                bits += 6;
                if (bits >= 8) { bits -= 8; decoded.push_back(static_cast<uint8_t>(accumulator >> bits)); }
            }
            if (!valid || decoded.empty() || bits == 6) continue;
            const auto type = decoded.front() & 31;
            if (type == 7) parameter_sets_.UpdateSps(decoded.data(), decoded.size());
            else if (type == 8) parameter_sets_.UpdatePps(decoded.data(), decoded.size());
        }
    }
}


bool H264Depacketizer::input(const RtpView& pkt)
{
    if (!pkt.valid())
    {
        return false;
    }

    media::H264ParsedPacket parsed;

    if (!parser_.Parse(pkt, parsed))
    {
        return false;
    }

    if (!parsed.valid)
    {
        return false;
    }

    auto result = packet_buffer_.InsertPacket(std::move(parsed));
    for (auto& frame : result.frames)
    {
        parameter_sets_.UpdateAccessUnit(frame);
        ready_frames_.emplace_back(std::move(frame));
    }

    return result.inserted || result.duplicate;
}


bool H264Depacketizer::hasFrame() const
{
    return !ready_frames_.empty();
}

std::vector<uint8_t> H264Depacketizer::popFrame()
{
    if (ready_frames_.empty())
    {
        return {};
    }
    media::H264AccessUnit au = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return au.ToAnnexB();
}

bool H264Depacketizer::popAccessUnit(media::H264AccessUnit& out)
{
    if (ready_frames_.empty())
        return false;
    out = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return true;
}
