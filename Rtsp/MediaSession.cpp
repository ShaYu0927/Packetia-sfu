#include "MediaSession.h"

#include "Rtp.h"
#include "RtspMessage.h"

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

bool MediaSession::AddTrack(TrackType type,
                            const std::string& codec,
                            const std::string& control,
                            int payload_type,
                            int clock_rate)
{
    
    for (auto& t : tracks_) 
    {
        if (t->_control == control) 
        {
            // 可选择更新 codec/pt/clock_rate
            t->_codec = codec;
            t->_pt = payload_type;
            t->_clock_rate = clock_rate;
            return true;
        }
    }

    auto track = std::make_shared<SdpTracker>();
    track->_type = static_cast<SdpTracker::TrackType>(type);
    track->_codec = codec;
    track->_control = control;
    track->_pt = payload_type;
    track->_clock_rate = clock_rate;

    track->_inited = false;   
    tracks_.push_back(track);

    LOG_INFO("AddTrack: type=" + std::to_string((int)type) +
             " control=" + control +
             " codec=" + codec +
             " pt=" + std::to_string(payload_type) +
             " clock=" + std::to_string(clock_rate) +
             " total=" + std::to_string(tracks_.size()));
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
    for (auto &client : clients_) 
    {
        if (!client.second->send_thread.joinable()) 
        {
            client.second->send_thread = std::thread(&MediaSession::SendLoop, this, client.second.get());
        }
    }
    if (clients_.empty()) 
    {
        LOG_ERROR("No clients connected to the media session.");
        return;
    }
}


void MediaSession::Stop()
{
    running_ = false;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto &client : clients_) 
    {
        client.second->running = false;
        client.second->queue_cv.notify_one();
        if (client.second->send_thread.joinable())
            client.second->send_thread.join();
    }
    clients_.clear();
}

void MediaSession::BindRtpTrack(int trackIdx, const std::shared_ptr<RtpTrack> &track)
{
    if(!track)
    {
        LOG_ERROR("BindRtpTrack: null track, idx=" + std::to_string(trackIdx));
        return;
    }
    std::lock_guard<std::mutex> lk(track_mtx_);
    rtp_tracks_[trackIdx] = track;


    LOG_INFO("BindRtpTrack: session=" + std::to_string(session_id_) +
             " idx=" + std::to_string(trackIdx) +
             " track_ptr=" + std::to_string(reinterpret_cast<uintptr_t>(track.get())));
}

std::shared_ptr<RtpTrack> MediaSession::GetRtpTrack(int trackIdx) const
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    auto it = rtp_tracks_.find(trackIdx);
    if(it == rtp_tracks_.end()) return nullptr;
    return it->second.lock();
}

void MediaSession::UnbindRtpTrack(int trackIdx)
{
    std::lock_guard<std::mutex> lk(track_mtx_);
    rtp_tracks_.erase(trackIdx);

    LOG_INFO("UnbindRtpTrack: session=" + std::to_string(session_id_) +
             " idx=" + std::to_string(trackIdx));
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

RtpTrackPtr MediaSessionManager::GetTrackByIndex(uint8_t track_index)
{
     std::lock_guard<std::mutex> lock(mtx_);
     auto it = _channel_to_track.find(track_index);
     return (it != _channel_to_track.end()) ? it->second : nullptr;
}

SdpTrackerPtr MediaSessionManager::GetTrackBySessionAndIndex(const std::string &session_id, int track_index)
{
    auto session = GetSessionById(session_id);
    if (session && track_index >= 0 && track_index < static_cast<int>(session->tracks_.size())) 
    {
        return session->tracks_[track_index];
    }
    return nullptr;
}

std::shared_ptr<RtpTrack> MediaSessionManager::GetTrackByChnnel(uint8_t channel)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = _channel_to_track.find(channel);
    if (it != _channel_to_track.end()) 
    {
        return it->second;
    }
    return nullptr;
}

int MediaSessionManager::GetTcpChannelByChannel(uint8_t channel)
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [key, tracker] : _channel_to_tcp_tracker_) 
    {
        if (tracker.rtp == channel || tracker.rtcp == channel) 
        {
            return key; 
        }
    }
    return -1; 
}

void MediaSessionManager::AddTcpChannelMapping(int tracker, const TcpChannel &tcp_channel)
{
    std::lock_guard<std::mutex> lock(mtx_);
    LOG_INFO("Mapping TCP channels for index: " + std::to_string(tracker) +
             " RTP channel=" + std::to_string(tcp_channel.rtp) +
             " RTCP channel=" + std::to_string(tcp_channel.rtcp));
    _channel_to_tcp_tracker_[tracker] = tcp_channel;
}
