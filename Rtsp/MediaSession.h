#ifndef _MEDIASESSION_H_
#define _MEDIASESSION_H_

#include <mutex>
#include <map>
#include "Media.h"
#include "MediaSource.h"
#include "RtpConnection.h"

class MediaSession {
public:
    using Ptr = std::shared_ptr<MediaSession>;
    std::string GetRtspSuffix() const;
    MediaSessionId GetId() const
    {
        return session_id_;
    }

    void AddSource(int client_fd, MediaSource::Ptr media_source);
    void RemoveSource(int client_fd);
    void PushFrame(MediaChannelId channel_id, AVFrame& frame);

    std::string GetSdpMessage(std::string ip, std::string session_name ="");

    uint32_t GetMediaChannelClockRate(MediaChannelId channel_id) const
    {
        
        return 0;
    }

    uint32_t GetMediaChannelPayloadType(MediaChannelId channel_id) const
    {
        
        return 0;
    }

    virtual MediaSession::Ptr LookMediaSession(const std::string& suffix) = 0;
    virtual MediaSession::Ptr LookMediaSession(MediaSessionId sessionId) = 0;

private:
    MediaSessionId session_id_ = 0;
	std::string suffix_;
	std::string sdp_;

    std::vector<std::unique_ptr<MediaSource::Ptr>> media_sources_; // 媒体源列表

    std::mutex mutex_;
	std::mutex map_mutex_;
	std::map<int, std::weak_ptr<RtpConnection>> clients_;  // 客户端连接列表，使用 weak_ptr 避免循环引用


    // 复合传输
    bool is_multicast_ = false;
	uint16_t multicast_port_[MAX_MEDIA_CHANNEL];
	std::string multicast_ip_;
	std::atomic_bool has_new_client_;

	static std::atomic_uint last_session_id_;

};


#endif // _MEDIASESSION_H_