#ifndef _MEDIASESSION_H_
#define _MEDIASESSION_H_

#include "Media.h"
#include "MediaSource.h"

class MediaSession {
public:
    using Ptr = std::shared_ptr<MediaSession>;
    std::string GetRtspSuffix() const;
    MediaSessionId GetId() const
    {
        return session_id_;
    }

    void AddClient(int client_fd, MediaChannelId channel_id);
    void RemoveClient(int client_fd);
    void PushFrame(MediaChannelId channel_id, AVFrame& frame);

    std::string GetSdpMessage(std::string ip, std::string session_name ="");

    uint32_t GetMediaChannelClockRate(MediaChannelId channel_id) const
    {
        if (channel_id < media_sources_.size()) {
            return media_sources_[channel_id]->GetClockRate();
        }
        return 0;
    }

    uint32_t GetMediaChannelPayloadType(MediaChannelId channel_id) const
    {
        if (channel_id < media_sources_.size()) {
            return media_sources_[channel_id]->GetPayload();
        }
        return 0;
    }

private:
    MediaSessionId session_id_ = 0;
	std::string suffix_;
	std::string sdp_;

    std::vector<MediaSource::Ptr> media_sources_; // 媒体源列表
};


#endif // _MEDIASESSION_H_