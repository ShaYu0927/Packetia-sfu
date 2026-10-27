#include "RtspMediaSession.h"
#include "MediaStreamRegistry.h"
#include "SdpTrackBinding.h"
#include "RtpTrackAdapter.h"
#include <utility>

namespace
{
std::string NormalizeControlKey(std::string value)
{
    const auto query = value.find_first_of("?#");
    if (query != std::string::npos) value.erase(query);
    while (!value.empty() && value.back() == '/') value.pop_back();
    const auto slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}
}

RtspMediaSession::RtspMediaSession(std::string suffix)
    : stream_(std::make_shared<media::MediaStream>(std::move(suffix))) {}

RtspMediaSession::Ptr RtspMediaSession::CreateNew(const std::string& suffix)
{
    return Ptr(new RtspMediaSession(suffix));
}

RtspMediaSession::~RtspMediaSession()
{
    Close();
}

void RtspMediaSession::Close()
{
    if (registered_) media::MediaStreamRegistry::Instance().Remove(stream_);
    registered_ = false;
}

bool RtspMediaSession::Register()
{
    if (registered_) return stream_->GetState() != media::MediaStream::State::Closed;
    registered_ = media::MediaStreamRegistry::Instance().Register(stream_);
    return registered_;
}

RtspMediaSession::Ptr RtspMediaSession::CreateNextPublication() const
{
    auto next = CreateNew(GetRtspSuffix());
    if (!next->stream_->ReplaceTracks(stream_->GetTracks())) return nullptr;
    next->control_to_track_ = control_to_track_;
    next->track_controls_ = track_controls_;
    next->selected_payloads_ = selected_payloads_;
    next->negotiated_ = negotiated_;
    return next;
}

bool RtspMediaSession::ApplySdp(const sdp::SdpSession& sdp, std::string* error)
{
    constexpr std::size_t kMaxTracks = 5;
    std::vector<media::MediaStreamTrack> tracks;
    std::unordered_map<std::string, int> controls;
    std::unordered_map<int, std::string> track_controls;
    std::vector<std::size_t> selections;
    auto negotiated = media::RtpStreamContextBuilder::BuildFromSdp(sdp);
    for (const auto& context : negotiated->tracks)
    {
        if (tracks.size() >= kMaxTracks)
        {
            if (error) *error = "too many tracks";
            return false;
        }
        const auto index = static_cast<int>(tracks.size());
        std::size_t selected = 0;
        media::MediaStreamTrack track;
        if (!media::SelectRtpPayload(context, selected, error) ||
            !media::DescribeMediaTrack(context, selected, track, error)) return false;
        auto control = NormalizeControlKey(sdp.medias[index].GetAttribute("control"));
        if (control.empty()) control = "trackID=" + std::to_string(index);
        if (!controls.emplace(control, index).second)
        {
            if (error) *error = "duplicate media control: " + control;
            return false;
        }
        track_controls.emplace(index, std::move(control));
        tracks.push_back(std::move(track));
        selections.push_back(selected);
    }
    // 全量验证后再提交；失败保留上一次可用描述，不留下半条流。
    if (!stream_->ReplaceTracks(std::move(tracks), error)) return false;
    control_to_track_ = std::move(controls);
    track_controls_ = std::move(track_controls);
    selected_payloads_ = std::move(selections);
    negotiated_ = std::move(negotiated);
    channels_ = {};
    return true;
}

bool RtspMediaSession::GetTrackInfo(int track_id, TrackInfo* out) const
{
    if (track_id < 0 || !out) return false;
    media::MediaStreamTrack track;
    if (!stream_->GetTrack(static_cast<media::TrackId>(track_id), &track)) return false;
    const auto index = static_cast<std::size_t>(track_id);
    if (!negotiated_ || index >= negotiated_->tracks.size() || index >= selected_payloads_.size()) return false;
    TrackInfo info;
    if (!media::ToLegacyRtpTrackInfo(negotiated_->tracks[index], selected_payloads_[index], info)) return false;
    const auto control = track_controls_.find(track_id);
    if (control != track_controls_.end()) info.control = control->second;
    *out = std::move(info);
    return true;
}

std::shared_ptr<RtpTrackDescription> RtspMediaSession::GetTrackDescription(const std::string& control) const
{
    const auto it = control_to_track_.find(NormalizeControlKey(control));
    if (it == control_to_track_.end()) return nullptr;
    TrackInfo info;
    return GetTrackInfo(it->second, &info) ? std::make_shared<RtpTrackDescription>(info) : nullptr;
}

bool RtspMediaSession::FindPayloadType(int track_id, uint8_t payload_type, media::RtpPayloadInfo* out) const
{
    if (!out || !negotiated_) return false;
    for (const auto& track : negotiated_->tracks)
        if (track.media_index == track_id)
            for (const auto& payload : track.payloads)
                if (payload.payload_type == payload_type) { *out = payload; return true; }
    return false;
}

bool RtspMediaSession::BindTrackEndpoint(int track_id, uint64_t endpoint_id, int rtp_channel, int rtcp_channel)
{
    const bool tcp = rtp_channel != -1 || rtcp_channel != -1;
    if (tcp && (rtp_channel < 0 || rtp_channel > 255 || rtcp_channel < 0 || rtcp_channel > 255 ||
        rtp_channel == rtcp_channel || channels_[rtp_channel].endpoint_id || channels_[rtcp_channel].endpoint_id))
        return false;
    if (track_id < 0 || !stream_->BindTrackEndpoint(static_cast<media::TrackId>(track_id), endpoint_id)) return false;
    if (tcp)
    {
        channels_[rtp_channel] = {endpoint_id, track_id};
        channels_[rtcp_channel] = {endpoint_id, track_id};
    }
    return true;
}

void RtspMediaSession::UnbindTrackEndpoint(uint64_t endpoint_id)
{
    stream_->UnbindTrackEndpoint(endpoint_id);
    for (auto& binding : channels_)
        if (binding.endpoint_id == endpoint_id) binding = {};
}
