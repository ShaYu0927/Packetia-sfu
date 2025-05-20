#ifndef _MEDIASESSION_H_
#define _MEDIASESSION_H_

#include "Media.h"

class MediaSession {
public:
    std::string GetRtspSuffix() const;
    MediaSessionId GetId() const;

    void AddClient(int client_fd, MediaChannelId channel_id);
    void RemoveClient(int client_fd);
    void PushFrame(MediaChannelId channel_id, AVFrame& frame);
};


#endif // _MEDIASESSION_H_