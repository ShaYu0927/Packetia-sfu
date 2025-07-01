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


using MediaSourcePtr = std::shared_ptr<MediaSource>;
using RtpPacketPtr = std::shared_ptr<RtpPacket>;

class MediaSession : public std::enable_shared_from_this<MediaSession>
{
public:
    using Ptr = std::shared_ptr<MediaSession>;
    MediaSession() = default;
    static Ptr CreateNew(const std::string& suffix);

    bool AddSource(MediaChannelId channel_id, MediaSourcePtr source);
    void PushFrame(MediaChannelId channel_id, const AVFrame& frame);
    std::string GetSdpMessage(const std::string& ip, const std::string& session_name = "");

    bool AddClient(int client_fd, std::shared_ptr<RtpConnection> conn);
    void RemoveClient(int client_fd);

    void Start();
    void Stop();

    MediaSessionId GetId() const { return session_id_; }
    const std::string& GetRtspSuffix() const { return suffix_; }

    MediaSessionId GetMediaChannelClockRate(MediaChannelId channel_id) const {
        // 这里可以根据实际情况返回媒体通道的时钟频率
        return 90000; // 默认值，实际应用中可能需要根据具体媒体类型返回不同的值
    }
    uint8_t GetMediaChannelPayloadType(MediaChannelId channel_id) const {
        // 这里可以根据实际情况返回媒体通道的负载类型
        return 96; // 默认值，实际应用中可能需要根据具体媒体类型返回不同的值
    }

    bool isMulticast() const {
        return is_multicast_;
    }

    std::string GetMulticastIp() const {
        return multicast_ip_;
    }

    uint16_t GetMulticastPort(MediaChannelId channel_id) const {
        if (channel_id >= MAX_MEDIA_CHANNEL) {
            return 0; // Invalid channel
        }
        return multicast_port_[channel_id];
    }


private:
    struct ClientSession {
        std::shared_ptr<RtpConnection> connection;
        std::deque<RtpPacketPtr> send_queue;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::thread send_thread;
        std::atomic_bool running{true};
    };

    //缓存队列包
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

    bool is_multicast_{false};
    std::string multicast_ip_;
    uint16_t multicast_port_[MAX_MEDIA_CHANNEL];
    std::atomic_bool has_new_client_;
};
