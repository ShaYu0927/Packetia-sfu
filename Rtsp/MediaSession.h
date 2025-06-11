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
#include "RtpConnection.h"

using MediaSessionId = uint64_t;
using MediaSourcePtr = std::shared_ptr<MediaSource>;
using RtpPacketPtr = std::shared_ptr<RtpPacket>;

class MediaSession : public std::enable_shared_from_this<MediaSession>
{
public:
    using Ptr = std::shared_ptr<MediaSession>;

    static Ptr CreateNew(const std::string& suffix);

    bool AddSource(MediaChannelId channel_id, MediaSourcePtr source);
    void PushFrame(MediaChannelId channel_id, const AVFrame& frame);
    std::string GetSdpMessage(const std::string& ip, const std::string& session_name = "");

    bool AddClient(int client_fd, std::shared_ptr<RtpConnection> conn);
    void RemoveClient(int client_fd);

    void Start();
    void Stop();

private:
    struct ClientSession {
        std::shared_ptr<RtpConnection> connection;
        std::deque<RtpPacketPtr> send_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::thread send_thread;
        std::atomic_bool running{true};
    };

    struct PacketCache {
        std::unordered_map<uint16_t, RtpPacketPtr> packets;
        std::mutex cache_mutex;
    };

    MediaSession(const std::string& suffix);
    void DispatchRtpPacket(MediaChannelId channel_id, RtpPacketPtr pkt);
    void SendLoop(ClientSession* client);

private:
    MediaSessionId session_id_{0};
    std::string suffix_;
    std::string sdp_;

    std::vector<MediaSourcePtr> media_sources_;
    PacketCache packet_cache_;

    std::mutex clients_mutex_;
    std::map<int, std::unique_ptr<ClientSession>> clients_;

    std::atomic_bool running_{false};

    static std::atomic_uint64_t last_session_id_;
};
