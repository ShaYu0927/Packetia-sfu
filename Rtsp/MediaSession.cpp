#include "MediaSession.h"

std::atomic_uint64_t MediaSession::last_session_id_{0};

MediaSession::Ptr MediaSession::CreateNew(const std::string& suffix)
 {
    return Ptr(new MediaSession(suffix));
 }


bool MediaSession::AddSource(MediaChannelId channel_id, MediaSourcePtr source)
{
    if(channel_id >= media_sources_.size())
    {
        LOG_ERROR("Invalid channel_id: " + std::to_string(channel_id));
        return false;
    }

    media_sources_[channel_id] = source;

    // source->SetFrameCallback([this, channel_id](const AVFrame& frame) {
    //     // 你这里可以封装为RtpPacket
    //     auto rtp_pkt = std::make_shared<RtpPacket>();
    //     rtp_pkt->FillFromFrame(frame);
    //     DispatchRtpPacket(channel_id, rtp_pkt);
    // });

    return false;
}

bool MediaSession::AddTrack(SdpTracker::TrackType type, const std::string &codec, const std::string &control)
{
    auto track = std::make_shared<SdpTracker>();
    track->_type = type;
    track->_codec = codec;
    track->_control = control;

    if(type == SdpTracker::TrackVideo) track->_pt = 96;  
    else if(type == SdpTracker::TrackAudio) track->_pt = 97;

    track->_inited = true;
    tracks_.push_back(track);
    return true;
}

// bool MediaSession::AddTrack(SdpTracker::TrackType type, const std::string &codec, const std::string &control)
// {
//     auto track = std::make_shared<SdpTracker>();
//     track->type = type;
//     track->codec = codec;
//     track->control = control;
//     sdp->AddTrack(track);
//     return true;
// }

void MediaSession::PushFrame(MediaChannelId channel_id, const AVFrame &frame)
{
    if (channel_id >= media_sources_.size()) return;
    if (media_sources_[channel_id])
        media_sources_[channel_id]->HanleFrame(channel_id,frame);
}


std::string MediaSession::GetSdpMessage(const std::string& ip, const std::string& session_name)
{
    return "";
}

bool MediaSession::AddClient(int client_fd, std::shared_ptr<RtpConnection> conn)
{
    auto client = std::make_unique<ClientSession>();
    client->connection = conn;
    client->send_thread = std::thread(&MediaSession::SendLoop, this, client.get());
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[client_fd] = std::move(client);
    }
    return true;
}

void MediaSession::RemoveClient(int client_fd)
{
    std::unique_ptr<ClientSession> client;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto iter = clients_.find(client_fd);
        if (iter != clients_.end()) 
        {
            client = std::move(iter->second);
            clients_.erase(iter);
        }
    }
    if (client) 
    {
        client->running = false;
        client->queue_cv.notify_one();
        if (client->send_thread.joinable())
            client->send_thread.join();
    }
}

//媒体从这里推流给客户端
void MediaSession::Start()
{
    running_ = true;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto &client : clients_) {
        if (!client.second->send_thread.joinable()) {
            client.second->send_thread = std::thread(&MediaSession::SendLoop, this, client.second.get());
        }
    }
    if (clients_.empty()) {
        LOG_ERROR("No clients connected to the media session.");
        return;
    }
}


void MediaSession::Stop()
{
    running_ = false;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto &client : clients_) {
        client.second->running = false;
        client.second->queue_cv.notify_one();
        if (client.second->send_thread.joinable())
            client.second->send_thread.join();
    }
    clients_.clear();
}


MediaSession::MediaSession(const std::string &suffix)
    : suffix_(suffix)
{
     session_id_ = ++last_session_id_;
     media_sources_.resize(MAX_MEDIA_CHANNEL);
}

//缓存RTP数据包
void MediaSession::DispatchRtpPacket(MediaChannelId channel_id, RtpPacketPtr pkt)
{
    
}

void MediaSession::SendLoop(ClientSession *client)
{
    while(client->running)
    {
        std::unique_lock<std::mutex> lock(client->queue_mutex);
        client->queue_cv.wait(lock, [&client] { return !client->send_queue.empty() || !client->running; });

        while (!client->send_queue.empty() && client->running) 
        {
            auto pkt = client->send_queue.front();
            client->send_queue.pop_front();
            lock.unlock();

          
        }
    }
}
