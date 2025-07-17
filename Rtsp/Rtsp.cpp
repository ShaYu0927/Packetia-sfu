#include "Rtsp.h"
#include "MediaSession.h"

std::string Sdp::GetSdpMessage(const std::string &ip, const std::string &session_name)
{
    
    if(media_list_.size() == 0)
    {
        return "";
    }

     std::ostringstream oss;
     oss << "v=0\r\n"
         << "o=- 0 0 IN IP4 " << ip << "\r\n"
         << "s=" << session_name << "\r\n"
         << "c=IN IP4 " << ip << "\r\n"
         << "t=0 0\r\n"
         << "a=control:*\r\n"
         << "a=range:npt=0-\r\n";

     for (const auto& media : media_list_) 
     {
        oss << "m=" << media.media_type << " " << media.port << " " << media.protocol
            << " " << media.payload_type << "\r\n";
        oss << "a=rtpmap:" << media.payload_type << " " << media.codec_name << "/" << media.clock_rate << "\r\n";
        if (!media.control.empty()) 
        {
            oss << "a=control:" << media.control << "\r\n";
         }
     }

     return oss.str();
}

void Sdp::AddMedia(const MediaDescription &media)
{
    media_list_.emplace_back(media);
}

void SdpParser::load(const std::string &sdp)
{
    _track_vec.clear();
    Sdp::Ptr sdp_ptr = std::make_shared<Sdp>();
    if (Parse(sdp, sdp_ptr)) {
        for (const auto &media : sdp_ptr->media_list_) {
            SdpTracker::Ptr track = std::make_shared<SdpTracker>();
            track->_t = media.media_type;
            track->_port = media.port;
            track->_codec = media.codec_name;
            track->_pt = media.payload_type;
            track->_control = media.control;
            _track_vec.push_back(track);
        }
    }
}

bool SdpParser::available() const
{
    return getTrack(SdpTracker::TrackType::TrackAudio) || getTrack(SdpTracker::TrackType::TrackVideo);
}

SdpTracker::Ptr SdpParser::getTrack(SdpTracker::TrackType type) const
{
    for (auto &track : _track_vec) {
        if ((type == SdpTracker::TrackType::TrackAudio && track->_t == "audio") ||
            (type == SdpTracker::TrackType::TrackVideo && track->_t == "video")) {
            return track;
        }
    }
    return nullptr;
}

bool SdpParser::Parse(const std::string &sdp_message, Sdp::Ptr &sdp) {
    std::istringstream iss(sdp_message);
    std::string line;
    Sdp::MediaDescription current;

    while (std::getline(iss, line)) {
        if (line.back() == '\r') line.pop_back(); // 清除 \r

        if (line.substr(0, 2) == "m=") {
            if (!current.media_type.empty()) {
                sdp->media_list_.push_back(current);  // 推入上一个
            }

            current = {};  // 新建
            std::istringstream lss(line.substr(2));
            lss >> current.media_type >> current.port >> current.protocol >> current.payload_type;
        }
        else if (line.substr(0, 9) == "a=rtpmap:") {
            size_t sep = line.find(' ');
            if (sep != std::string::npos) {
                int pt = std::stoi(line.substr(9, sep - 9));
                std::string codec_info = line.substr(sep + 1);
                size_t slash = codec_info.find('/');
                if (slash != std::string::npos) {
                    current.codec_name = codec_info.substr(0, slash);
                    current.clock_rate = std::stoi(codec_info.substr(slash + 1));
                }
            }
        }
        else if (line.substr(0, 9) == "a=control") {
            size_t pos = line.find(":");
            if (pos != std::string::npos)
                current.control = line.substr(pos + 1);
        }
    }

    // 推入最后一个
    if (!current.media_type.empty()) {
        sdp->media_list_.push_back(current);
    }

    return !sdp->media_list_.empty();
}


std::vector<SdpTracker::Ptr> SdpParser::getAvailableTrack() const
{
    std::vector<SdpTracker::Ptr> available_tracks;
    for (const auto &track : _track_vec) {
        if (track->_t == "audio" || track->_t == "video") {
            available_tracks.push_back(track);
        }
    }
    return available_tracks;
}

std::string SdpParser::toString() const
{
    std::ostringstream oss;
    for (const auto &track : _track_vec) {
        oss << "Type: " << track->_t
            << ", Port: " << track->_port
            << ", Codec: " << track->_codec
            << ", Payload Type: " << track->_pt
            << ", Control: " << track->_control
            << "\n";
    }
    return oss.str();
}
// 拼接完成url
std::string SdpParser::getControlUrl(const std::string &url) const
{
    for (const auto &track : _track_vec) {
        if (!track->_control.empty()) {
            if (track->_control.find("rtsp://") == 0) {
                // 已经是完整的 URL
                return track->_control;
            } else if (track->_control.front() == '/') {
                return url + track->_control;  // 相对路径（绝对形式）
            } else {
                return url + "/" + track->_control;  // 相对路径
            }
        }
    }
    return url; // fallback
}

bool Rtsp::AddMediaSession(const MediaSession::Ptr &session)
{
     const auto& suffix = session->GetRtspSuffix();
    if (media_sessions_.find(suffix) != media_sessions_.end()) {
        LOG_INFO("MediaSession already exists: " + suffix);
        return false;
    }

    media_sessions_[suffix] = session;
    LOG_INFO("Added MediaSession: " + suffix);
    return true;
}
