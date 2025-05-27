#ifndef _MEDIASESSION_H_
#define _MEDIASESSION_H_

#include "Media.h"
#include "MediaSource.h"

class MediaSession {
public:
    using Ptr = std::shared_ptr<MediaSession>;
    std::string GetRtspSuffix() const;
    MediaSessionId GetId() const;

    void AddClient(int client_fd, MediaChannelId channel_id);
    void RemoveClient(int client_fd);
    void PushFrame(MediaChannelId channel_id, AVFrame& frame);

    std::string GetSdpMessage(std::string ip, std::string session_name ="");

private:
    MediaSessionId session_id_ = 0;
	std::string suffix_;
	std::string sdp_;

    std::vector<MediaSource::Ptr> media_sources_; // 媒体源列表
};


#endif // _MEDIASESSION_H_