#include "RtspMediaSession.h"
#include "SdpTrackBinding.h"
#include "SdpMode.h"
#include "RtspUtil.h"
#include "StreamContext.h"

#include <algorithm>
#include <cstdlib>

namespace
{
std::string NormalizeControlKey(const std::string& control)
{
    if (control.empty())
    {
        return {};
    }

    std::string value = control;
    const auto query = value.find_first_of("?#");
    if (query != std::string::npos)
    {
        value.erase(query);
    }

    while (!value.empty() && value.back() == '/')
    {
        value.pop_back();
    }

    const auto slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

}

static uint64_t MakeStreamKey(uint32_t session_id, uint32_t track_id)
{
    return (static_cast<uint64_t>(session_id) << 32) |
           static_cast<uint32_t>(track_id);
}

MediaSession::Ptr MediaSession::CreateNew(const std::string& suffix)
{
    auto session = std::make_shared<MediaSession>();
    session->suffix_ = suffix;
    return session;
}

std::string MediaSessionManager::GenerateGlobalId()
{
    static std::atomic<uint64_t> seq{0};

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    uint64_t n = ++seq;

    std::ostringstream oss;
    oss << now << "-" << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << "-" << n;
    return oss.str();
}

uint32_t MediaSessionManager::AddSession(MediaSession::Ptr session, const std::string& suffix) 
{
    if (!session)
        return 0;

    std::lock_guard<std::mutex> lock(mtx_);

    if (suffix.empty())
        return 0;

    if (suffix_map_.find(suffix) != suffix_map_.end())
        return 0;

    uint32_t id = ++last_id_;
    session->session_id_ = id;
    session->frame_publisher_ = frame_publisher_;

    if (session->global_id_.empty())
        session->global_id_ = GenerateGlobalId();

    sessions_[id] = session;
    suffix_map_[suffix] = session;
    return id;
}

void MediaSessionManager::SetFramePublisher(std::shared_ptr<media::IEncodedFramePublisher> publisher)
{
    std::lock_guard<std::mutex> lock(mtx_);
    frame_publisher_ = std::move(publisher);
    for (auto& entry : sessions_)
    {
        entry.second->SetFramePublisher(frame_publisher_);
    }
}

void MediaSession::SetFramePublisher(std::shared_ptr<media::IEncodedFramePublisher> publisher)
{
    std::lock_guard<std::mutex> lock(track_mtx_);
    frame_publisher_ = std::move(publisher);
}

std::shared_ptr<media::IEncodedFramePublisher> MediaSession::GetFramePublisher() const
{
    std::lock_guard<std::mutex> lock(track_mtx_);
    return frame_publisher_;
}

MediaSession::Ptr MediaSessionManager::GetSessionById(const uint32_t& id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

MediaSession::Ptr MediaSessionManager::GetSessionBySuffix(const std::string& suffix)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = suffix_map_.find(suffix);
    return (it != suffix_map_.end()) ? it->second : nullptr;
}

void MediaSessionManager::RemoveSession(const uint32_t& id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) 
    {
        for (auto sit = suffix_map_.begin(); sit != suffix_map_.end();) 
        {
            if (sit->second == it->second) 
            {
                sit = suffix_map_.erase(sit);
            } 
            else 
            {
                ++sit;
            }
        }
        sessions_.erase(it);
    }
}

void MediaSession::ResetTracks()
{
    track_infos_.clear();
    control_to_track_.clear();
    track_descriptions_.clear();
    track_to_endpoint_.clear();
    ssrc_to_track_.clear();
    channel_bindings_ = {};
    stream_context_.reset();
    sdp_.clear();
}

bool MediaSession::ParseTrackInfoFromMedia(const sdp::SdpMedia& media, int track_index, TrackInfo* info, std::string* err) const
{
    if (!info) {
        if (err) *err = "invalid TrackInfo output";
        return false;
    }
    if (!media::BuildRtpTrackInfo(media, track_index, *info, err)) return false;
    info->control = NormalizeControlKey(info->control);
    if (info->control.empty()) info->control = "trackID=" + std::to_string(track_index);
    return true;
}


RtpTrackDescription::Ptr MediaSession::CreateTrackDescription(const TrackInfo& info)
{
    switch (info.type)
    {
    case TrackType::TrackAudio:
        switch (info.codec_id)
        {
        case CodecId::PCMU:
        case CodecId::PCMA:
        case CodecId::OPUS:
        case CodecId::AAC:
            return std::make_shared<RtpTrackDescription>(info);

        default:
            return nullptr;
        }

    case TrackType::TrackVideo:
        switch (info.codec_id)
        {
        case CodecId::H264:
            return std::make_shared<RtpTrackDescription>(info);

        default:
            return nullptr;
        }

    default:
        return nullptr;
    }
}

bool MediaSession::ApplySdp(const sdp::SdpSession& sdp, std::string* err)
{
    std::lock_guard<std::mutex> lock(track_mtx_);

    ResetTracks();
    stream_context_ = StreamContextBuilder::BuildFromSdp(sdp, suffix_, url_);

    int track_idx = 0;
    for (const auto& media : sdp.medias)
    {
        if (track_idx >= MAX_TRACKS)
        {
            if (err)
            {
                *err = "too many tracks";
            }
            return false;
        }

        TrackInfo info;
        if (!ParseTrackInfoFromMedia(media, track_idx, &info, err))
        {
            return false;
        }

        if (info.type == TrackType::TrackInvalid)
        {
            continue;
        }

        auto track = BuildTrackDescription(info, err);
        if (!track)
        {
            return false;
        }

        LOG_INFO("track_idx=", track_idx,
                 " control=", info.control,
                 " codec=", info.codec_name,
                 " pt=", static_cast<int>(info.payload_type),
                 " clock_rate=", info.clock_rate,
                 " twcc_ext_id=", static_cast<int>(info.transport_cc_extension_id));

        track_infos_[track_idx] = info;
        const std::string control_key = NormalizeControlKey(info.control);
        if (control_to_track_.find(control_key) != control_to_track_.end())
        {
            if (err)
            {
                *err = "duplicate media control: " + control_key;
            }
            ResetTracks();
            return false;
        }
        control_to_track_[control_key] = track_idx;
        track_descriptions_[track_idx] = track;

        ++track_idx;
    }
    return true;
}

RtpTrackDescription::Ptr MediaSession::BuildTrackDescription(const TrackInfo& info, std::string* err)
{
    auto description = CreateTrackDescription(info);
    if (!description && err)
    {
        *err = "no receiver implementation for codec '" + info.codec_name +
               "' on track index " + std::to_string(info.track_index);
    }
    return description;
}


std::shared_ptr<RtpTrackDescription> MediaSession::GetTrackDescription(const std::string& control) const
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    auto it = control_to_track_.find(NormalizeControlKey(control));
    if (it == control_to_track_.end())
        return nullptr;

    int trackIdx = it->second;

    auto track_it = track_descriptions_.find(trackIdx);
    if (track_it == track_descriptions_.end())
        return nullptr;

    return track_it->second;
}

bool MediaSession::GetTrackInfo(int track_id, TrackInfo* out) const
{
    if (!out)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(track_mtx_);
    auto it = track_infos_.find(track_id);
    if (it == track_infos_.end())
    {
        return false;
    }

    *out = it->second;
    return true;
}

std::shared_ptr<const StreamContext> MediaSession::GetStreamContext() const
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    return stream_context_;
}

bool MediaSession::FindStreamTrack(int media_index, StreamTrackInfo* out) const
{
    if (!out)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(track_mtx_);
    if (!stream_context_ || media_index < 0)
    {
        return false;
    }

    for (const auto& track : stream_context_->tracks)
    {
        if (track.media_index == media_index)
        {
            *out = track;
            return true;
        }
    }

    return false;
}

bool MediaSession::FindStreamTrackByChannel(uint8_t channel, StreamTrackInfo* out) const
{
    if (!out)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(track_mtx_);
    if (!stream_context_)
    {
        return false;
    }

    auto it = stream_context_->channel_to_media_index.find(channel);
    if (it == stream_context_->channel_to_media_index.end())
    {
        return false;
    }

    const int media_index = it->second;
    for (const auto& track : stream_context_->tracks)
    {
        if (track.media_index == media_index)
        {
            *out = track;
            return true;
        }
    }

    return false;
}

bool MediaSession::FindPayloadType(uint8_t payload_type, PayloadTypeInfo* out) const
{
    if (!out)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(track_mtx_);
    if (!stream_context_)
    {
        return false;
    }

    auto it = stream_context_->payload_type_map.find(payload_type);
    if (it == stream_context_->payload_type_map.end())
    {
        return false;
    }

    *out = it->second;
    return true;
}

bool MediaSession::FindPayloadType(int track_id, uint8_t payload_type, PayloadTypeInfo* out) const
{
    if (!out)
    {
        return false;
    }

    std::lock_guard<std::mutex> lk(track_mtx_);
    if (!stream_context_)
    {
        return false;
    }

    for (const auto& track : stream_context_->tracks)
    {
        if (track.media_index != track_id)
        {
            continue;
        }

        for (const auto& payload : track.payloads)
        {
            if (payload.payload_type == payload_type)
            {
                *out = payload;
                return true;
            }
        }
        return false;
    }
    return false;
}

bool MediaSession::BindInterleavedChannel(uint8_t channel, int track_id, bool is_rtcp, uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lk(track_mtx_);

    auto it = track_descriptions_.find(track_id);
    if (it == track_descriptions_.end())
        return false;

    ChannelBinding& b = channel_bindings_[channel];
    b.valid = true;
    b.track_id = track_id;
    b.is_rtcp = is_rtcp;
    b.key = MakeStreamKey(session_id_, static_cast<uint32_t>(track_id));
    b.endpoint_id = endpoint_id;

    if (stream_context_)
    {
        stream_context_->channel_to_media_index[channel] = track_id;
        if (track_id >= 0)
        {
            for (auto& track : stream_context_->tracks)
            {
                if (track.media_index != track_id)
                {
                    continue;
                }

                if (is_rtcp)
                {
                    track.rtcp_channel = channel;
                }
                else
                {
                    track.rtp_channel = channel;
                }
                break;
            }
        }
    }
    return true;
}


bool MediaSession::GetChannelBinding(uint8_t channel, ChannelBinding* out) const
{
    if (!out) return false;

    std::lock_guard<std::mutex> lk(track_mtx_);
    const auto& b = channel_bindings_[channel];
    if (!b.valid) return false;

    *out = b;
    return true;
}

bool MediaSession::BindTrackEndpoint(int track_id, uint64_t endpoint_id, int rtp_channel, int rtcp_channel)
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    if (track_id < 0 || endpoint_id == 0 || track_to_endpoint_.count(track_id) || !track_descriptions_.count(track_id)) return false;
    const bool tcp = rtp_channel != -1 || rtcp_channel != -1;
    if (tcp && (rtp_channel < 0 || rtp_channel > 255 || rtcp_channel < 0 || rtcp_channel > 255 ||
                rtp_channel == rtcp_channel || channel_bindings_[rtp_channel].valid ||
                channel_bindings_[rtcp_channel].valid)) return false;
    track_to_endpoint_[track_id] = endpoint_id;
    if (tcp) {
        for (int channel : {rtp_channel, rtcp_channel}) {
            auto& binding = channel_bindings_[channel];
            binding.valid = true;
            binding.track_id = track_id;
            binding.is_rtcp = channel == rtcp_channel;
            binding.endpoint_id = endpoint_id;
            binding.key = MakeStreamKey(session_id_, static_cast<uint32_t>(track_id));
            if (stream_context_) stream_context_->channel_to_media_index[channel] = track_id;
        }
        if (stream_context_) for (auto& track : stream_context_->tracks) {
            if (track.media_index != track_id) continue;
            track.rtp_channel = rtp_channel;
            track.rtcp_channel = rtcp_channel;
        }
    }
    return true;
}

uint64_t MediaSession::FindEndpointByTrack(int track_id) const
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    const auto it = track_to_endpoint_.find(track_id);
    return it != track_to_endpoint_.end() ? it->second : 0;
}

void MediaSession::UnbindTrackEndpoint(uint64_t endpoint_id)
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    for (auto it = track_to_endpoint_.begin(); it != track_to_endpoint_.end();) {
        if (it->second == endpoint_id) it = track_to_endpoint_.erase(it);
        else ++it;
    }
    for (size_t channel = 0; channel < channel_bindings_.size(); ++channel) {
        auto& binding = channel_bindings_[channel];
        if (!binding.valid || binding.endpoint_id != endpoint_id) continue;
        if (stream_context_) stream_context_->channel_to_media_index.erase(channel);
        binding = ChannelBinding{};
    }
}
