#pragma once

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include "MediaSource.h"
#include "Rtp.h"
#include "RtpTypes.h"



#define MAX_TRACKS 5

class RtpTrack;
class RtpConnection;
class SdpTracker;
using RtpTrackPtr = std::shared_ptr<RtpTrack>;
using RtpConnectionPtr = std::shared_ptr<RtpConnection>;
using SdpTrackerPtr = std::shared_ptr<SdpTracker>;




using MediaSourcePtr = std::shared_ptr<MediaSource>;
using RtpPacketPtr = std::shared_ptr<RtpPacket>;

class MediaSession : public std::enable_shared_from_this<MediaSession>
{
public:
    using Ptr = std::shared_ptr<MediaSession>;
    MediaSession() = default;
    static Ptr CreateNew(const std::string& suffix);
    
    bool AddSource(MediaChannelId channel_id, MediaSourcePtr source);
    bool AddTrack(TrackType type, const std::string& codec, const std::string& control);
    void PushFrame(MediaChannelId channel_id, const AVFrame& frame);
    std::string GetSdpMessage(const std::string& ip, const std::string& session_name = "");

    bool AddClient(int client_fd, std::shared_ptr<RtpConnection> conn);
    void RemoveClient(int client_fd);

    void Start();
    void Stop();

    MediaSessionId GetId() const { return session_id_; }
    void SetId(MediaSessionId id) { session_id_ = id; }
    const std::string& GetRtspSuffix() const { return suffix_; }

    MediaSessionId GetMediaChannelClockRate(MediaChannelId channel_id) const 
    {
       
        return 90000; 
    }
    uint8_t GetMediaChannelPayloadType(MediaChannelId channel_id) const 
    {
        
        return 96; 
    }

    bool isMulticast() const 
    {
        return is_multicast_;
    }

    std::string GetMulticastIp() const 
    {
        return multicast_ip_;
    }

    uint16_t GetMulticastPort(MediaChannelId channel_id) const 
    {
        if (channel_id >= MAX_MEDIA_CHANNEL) 
        {
            return 0; // Invalid channel
        }
        return multicast_port_[channel_id];
    }

   

private:
    struct ClientSession 
    {
        std::shared_ptr<RtpConnection> connection;
        std::deque<RtpPacketPtr> send_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::thread send_thread;
        std::atomic_bool running{true};

        ~ClientSession() 
        {
            running = false;
            queue_cv.notify_all();

            if (send_thread.joinable()) 
            {
                send_thread.join();
            }
        }
    };

    //缓存队列包
    struct PacketCache {
        std::unordered_map<uint16_t, RtpPacketPtr> packets;
        std::mutex cache_mutex;
    };

    MediaSession(const std::string& suffix);
    void DispatchRtpPacket(MediaChannelId channel_id, RtpPacketPtr pkt);
    void SendLoop(ClientSession* client);
public:
    friend class MediaSessionManager; 
    MediaSessionId session_id_{0};
    std::vector<SdpTrackerPtr> tracks_;

private:
    
    std::string suffix_;
    std::string sdp_;

    std::vector<MediaSourcePtr> media_sources_;
    PacketCache packet_cache_;

    std::mutex clients_mutex_;
    std::map<int, std::unique_ptr<ClientSession>> clients_;
    

    std::atomic_bool running_{false};

    static std::atomic_uint64_t last_session_id_;

    // 组播
    bool is_multicast_{false};
    std::string multicast_ip_;
    uint16_t multicast_port_[MAX_MEDIA_CHANNEL];
    
    std::atomic_bool has_new_client_;
};


class MediaSessionManager {
public:
    using Ptr = std::shared_ptr<MediaSession>;

    static MediaSessionManager& Instance() {
        static MediaSessionManager inst;
        return inst;
    }

    std::string AddSession(MediaSession::Ptr session, const std::string& suffix) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string id = std::to_string(++last_id_);
        session->session_id_ = last_id_; // 给 session 分配全局 ID
        sessions_[id] = session;
        suffix_map_[suffix] = session;
        return id;
    }

    void AddTrackChannel(uint8_t track_index, const std::shared_ptr<RtpTrack>& track) {
        std::lock_guard<std::mutex> lock(mtx_);
        _channel_to_track[track_index] = track;
    }

    MediaSession::Ptr GetSessionById(const std::string& id) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sessions_.find(id);
        return (it != sessions_.end()) ? it->second : nullptr;
    }

    RtpTrackPtr GetTrackByIndex(uint8_t track_index); 

    MediaSession::Ptr GetSessionBySuffix(const std::string& suffix) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = suffix_map_.find(suffix);
        return (it != suffix_map_.end()) ? it->second : nullptr;
    }

    void RemoveSession(const std::string& id) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            // 同时从 suffix_map_ 删除
            for (auto sit = suffix_map_.begin(); sit != suffix_map_.end();) {
                if (sit->second == it->second) {
                    sit = suffix_map_.erase(sit);
                } else {
                    ++sit;
                }
            }
            sessions_.erase(it);
        }
    }

    // 获取 sdp tracker info
    SdpTrackerPtr GetTrackBySessionAndIndex(const std::string& session_id, int track_index); 

private:
    std::mutex mtx_;
    std::unordered_map<std::string, MediaSession::Ptr> sessions_;      // id -> session
    std::unordered_map<std::string, MediaSession::Ptr> suffix_map_;    // suffix -> session
    std::unordered_map<uint8_t, std::shared_ptr<RtpTrack>> _channel_to_track; // track index -> track Ptr
    std::atomic_uint64_t last_id_{0};
};
