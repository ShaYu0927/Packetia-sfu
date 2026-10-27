#ifndef PACKETIA_RTSP_RTSPMEDIASESSION_H_
#define PACKETIA_RTSP_RTSPMEDIASESSION_H_

#include "Rtp.h"
#include "Sdp.h"
#include "RtpStreamContext.h"
#include "MediaStream.h"
#include <array>
#include <memory>
#include <string>
#include <unordered_map>

// 保存 RTSP/RTP 协商参数和 channel 绑定；通用轨道描述及 Endpoint 归属由 core 管理。
// 所有方法在所属连接的 EventLoop 上串行调用，不跨线程共享此适配对象。
class RtspMediaSession
{
public:
    using Ptr = std::shared_ptr<RtspMediaSession>;
    static Ptr CreateNew(const std::string& suffix);
    ~RtspMediaSession();
    RtspMediaSession(const RtspMediaSession&) = delete;
    RtspMediaSession& operator=(const RtspMediaSession&) = delete;
    uint32_t GetId() const { return stream_->GetId(); }
    const std::string& GetRtspSuffix() const { return stream_->GetName(); }
    media::MediaStream::Ptr GetMediaStream() const { return stream_; }
    bool Register();
    void Close();
    Ptr CreateNextPublication() const;
    bool ApplySdp(const sdp::SdpSession& sdp, std::string* error);
    std::shared_ptr<RtpTrackDescription> GetTrackDescription(const std::string& control) const;
    bool GetTrackInfo(int track_id, TrackInfo* out) const;
    bool FindPayloadType(int track_id, uint8_t payload_type, media::RtpPayloadInfo* out) const;
    bool BindTrackEndpoint(int track_id, uint64_t endpoint_id, int rtp_channel = -1, int rtcp_channel = -1);
    void UnbindTrackEndpoint(uint64_t endpoint_id);

private:
    struct ChannelBinding
    {
        uint64_t endpoint_id = 0;
        int track_id = -1;
    };
    explicit RtspMediaSession(std::string suffix);
    media::MediaStream::Ptr stream_;
    bool registered_ = false;
    std::unordered_map<std::string, int> control_to_track_;
    std::unordered_map<int, std::string> track_controls_;
    // 对应 negotiated_->tracks，选中项引用快照中的 payload，不另存接收参数副本。
    std::vector<std::size_t> selected_payloads_;
    std::shared_ptr<const media::RtpStreamContext> negotiated_;
    std::array<ChannelBinding, 256> channels_{};
};

#endif
