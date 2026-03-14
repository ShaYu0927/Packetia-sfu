#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "Rtp.h"
#include "RtpTypes.h"
#include "Rtsp.h"
#include "Sdp.h"


#define MAX_TRACKS 5


class MediaSession : public std::enable_shared_from_this<MediaSession>
{
public:
    struct MediaTrackInfo
    {
        bool valid = false;
        TrackType type = TrackType::TrackInvalid;

        std::string codec;
        std::string control;
        int payload_type = -1;
        int clock_rate = 0;
        int channels = 0;
        std::string fmtp;
    };

    using Ptr = std::shared_ptr<MediaSession>;

    MediaSession() = default;
    static Ptr CreateNew(const std::string& suffix);

    bool AddTrack(TrackType type,
                        const std::string& codec,
                        const std::string& control,
                        int payload_type,
                        int clock_rate);

    MediaSessionId GetId() const { return session_id_; }
    void SetId(MediaSessionId id) { session_id_ = id; }
    const std::string& GetRtspSuffix() const { return suffix_; }
    void SetRtspSuffix(const std::string& suffix) { suffix_ = suffix; }
    
    void BindRtpTrack(int trackIdx, const std::shared_ptr<RtpTrack>& track);
    std::shared_ptr<RtpTrack> GetRtpTrack(int trackIdx) const;
    void UnbindRtpTrack(int trackIdx);

    bool ApplySdp(const sdp::SdpSession& sdp, std::string* err);


    bool IsReady() const { return ready_.load(); }
    void SetReady(bool ready) { ready_.store(ready); }

    bool HasPublisher() const { return has_publisher_; }
    void SetHasPublisher(bool v) { has_publisher_ = v; }

public:
    friend class MediaSessionManager;

    

private:
    std::string suffix_;
    std::string sdp_;
    MediaSessionId session_id_{0};
    std::string global_id_;

    mutable std::mutex track_mtx_;
    std::atomic_bool ready_{false};
    bool has_publisher_{false};

    std::unordered_map<int, MediaTrackInfo> track_infos_;
    std::unordered_map<std::string, int> control_to_track_;
    std::unordered_map<int, std::weak_ptr<RtpTrack>> runtime_tracks_;
    std::unordered_map<uint32_t, int> ssrc_to_track_;

};


class MediaSessionManager 
{
public:
    using Ptr = std::shared_ptr<MediaSession>;

    static MediaSessionManager& Instance() 
    {
        static MediaSessionManager inst;
        return inst;
    }

    uint32_t AddSession(MediaSession::Ptr session, const std::string& suffix);
    MediaSession::Ptr GetSessionById(const uint32_t& id);
    MediaSession::Ptr GetSessionBySuffix(const std::string& suffix);
    void RemoveSession(const uint32_t& id);

    static std::string GenerateGlobalId();


private:
    std::mutex mtx_;
    std::unordered_map<uint32_t, MediaSession::Ptr> sessions_;       // id -> session
    std::unordered_map<std::string, MediaSession::Ptr> suffix_map_; // suffix -> session
    std::atomic_uint64_t last_id_{0};
};
