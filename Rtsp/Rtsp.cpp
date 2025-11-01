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

RtspSessionDesc Sdp::parse(const std::string &sdp)
{
    RtspSessionDesc session;
    std::istringstream iss(sdp);
    std::string line;
    std::shared_ptr<RtpTrackInfo> currentTrack;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.rfind("v=", 0) == 0) {
            session.version = line.substr(2);
        } else if (line.rfind("o=", 0) == 0) {
            session.origin = line.substr(2);
        } else if (line.rfind("s=", 0) == 0) {
            session.session_name = line.substr(2);
        } else if (line.rfind("c=", 0) == 0) {
            session.connection = line.substr(2);
        } else if (line.rfind("t=", 0) == 0) {
            session.timing = line.substr(2);
        } else if (line.rfind("a=tool:", 0) == 0) {
            session.tool = line.substr(7);
        } else if (line.rfind("m=", 0) == 0) {
            // 新建 track
            currentTrack = std::make_shared<RtpTrackInfo>();
            std::istringstream ms(line.substr(2));
            std::string media_type, protocol;
            ms >> media_type;      // video / audio
            ms >> currentTrack->payload_type; // port (其实常为0)
            ms >> protocol;        // RTP/AVP
            ms >> currentTrack->payload_type;
        } else if (line.rfind("a=rtpmap:", 0) == 0 && currentTrack) {
            currentTrack->rtpmap = line.substr(2);
            size_t sep = line.find(' ');
            if (sep != std::string::npos) {
                int pt = std::stoi(line.substr(9, sep - 9));
                std::string codec_info = line.substr(sep + 1);
                size_t slash = codec_info.find('/');
                if (slash != std::string::npos) {
                    currentTrack->codec = codec_info.substr(0, slash);
                    currentTrack->clock_rate = std::stoi(codec_info.substr(slash + 1));
                    // 如果还有第二个斜杠 -> 表示声道数
                    size_t slash2 = codec_info.find('/', slash + 1);
                    if (slash2 != std::string::npos) {
                        currentTrack->channels = std::stoi(codec_info.substr(slash2 + 1));
                    }
                }
            }
        } else if (line.rfind("a=fmtp:", 0) == 0 && currentTrack) {
            currentTrack->fmtp = line.substr(2);
            // 提取 vps/sps/pps 或 AAC config
            if (currentTrack->codec == "H265") {
                size_t vps_pos = line.find("sprop-vps=");
                if (vps_pos != std::string::npos) {
                    size_t end = line.find(';', vps_pos);
                    currentTrack->vps = line.substr(vps_pos + 10, end - (vps_pos + 10));
                }
                size_t sps_pos = line.find("sprop-sps=");
                if (sps_pos != std::string::npos) {
                    size_t end = line.find(';', sps_pos);
                    currentTrack->sps = line.substr(sps_pos + 10, end - (sps_pos + 10));
                }
                size_t pps_pos = line.find("sprop-pps=");
                if (pps_pos != std::string::npos) {
                    size_t end = line.find(';', pps_pos);
                    currentTrack->pps = line.substr(pps_pos + 10, end - (pps_pos + 10));
                }
            } else if (currentTrack->codec == "MPEG4-GENERIC") {
                size_t cfg_pos = line.find("config=");
                if (cfg_pos != std::string::npos) {
                    currentTrack->audio_config = line.substr(cfg_pos + 7);
                }
            }
        } else if (line.rfind("a=control:", 0) == 0 && currentTrack) {
            currentTrack->control = line.substr(10);
            session.tracks.push_back(*currentTrack);
            currentTrack.reset();
        }
    }

    return session;
}

void Sdp::Parse()
{
}

std::string Sdp::buildANNOUNCEBody()
{
    std::ostringstream oss;

    // 基础 SDP 信息
    oss << "v=0\r\n";
    oss << "o=- 0 0 IN IP4 127.0.0.1\r\n";
    oss << "s=No Name\r\n";
    oss << "c=IN IP4 127.0.0.1\r\n";
    oss << "t=0 0\r\n";

    if(media_list_.size() == 0)
    {
        return "";
    }

    for (const auto& m : media_list_)
    {
        oss << "m=" << m.media_type << " " << m.port << " RTP/AVP " << m.payload_type << "\r\n";
        oss << "a=rtpmap:" << m.payload_type << " " << m.codec_name << "/" << m.clock_rate << "\r\n";
        if (!m.codec_name.empty())
        {
            oss << "a=fmtp:" << m.payload_type << " "; 
            if (m.codec_name == "H265") {
                oss << "sprop-vps=xxx; sprop-sps=xxx; sprop-pps=xxx";
            } else if (m.codec_name == "MPEG4-GENERIC") {
                oss << "config=1210";
            }
            oss << "\r\n";
        }
        oss << "a=control:" << m.control << "\r\n";
    }

    return oss.str();
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

std::shared_ptr<MediaSession> Rtsp::LookMediaSession(const std::string& suffix)
{
    auto it = media_sessions_.find(suffix);
    if (it != media_sessions_.end())
        return it->second;
    return nullptr;
}

std::shared_ptr<MediaSession> Rtsp::LookMediaSession(MediaSessionId sessionId)
{
    for (auto& kv : media_sessions_)
    {
        if (kv.second->GetId() == sessionId)
            return kv.second;
    }
    return nullptr;
}


bool Rtsp::AddMediaSession(const std::shared_ptr<MediaSession>& session)
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


