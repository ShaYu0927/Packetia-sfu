#include "RtspConnection.h"
#include "RtpTypes.h"
#include "logger.h"


RtspConnection::RtspConnection(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd)
    , rtsp_server_(rtsp_server)
    ,task_scheduler_(task_scheduler)
{
    LOG_INFO("RtspConnection created with sockfd: " + std::to_string(sockfd));
    active_ = true;
}

std::shared_ptr<RtspConnection> RtspConnection::Create(std::shared_ptr<RtspServer> rtsp_server, TaskScheduler *task_scheduler, SOCKET sockfd)
{
    auto conn = std::shared_ptr<RtspConnection>(new RtspConnection(rtsp_server, task_scheduler, sockfd));

    conn->InitCallbacks();
    conn->Start();

    return conn;
}

RtspConnection::~RtspConnection()
{
}

void RtspConnection::InitCallbacks()
{
    std::weak_ptr<RtspConnection> weak_self =
    std::static_pointer_cast<RtspConnection>(TcpConnection::shared_from_this());


    this->SetReadCallback([weak_self](std::shared_ptr<TcpConnection>, BufferReader& buffer) -> bool {
        auto self = weak_self.lock();
        if (!self) return false;         
        return self->onRead(buffer);
    });

    
}

void RtspConnection::OnMessage(BufferReader *buffer)
{
}


void RtspConnection::SendRtspMessage(std::shared_ptr<char> data, uint32_t size)
{
    LOG_DEBUG("RTSP message content (" + std::to_string(size) + " bytes):");
    LOG_DEBUG("\n" + std::string(data.get(), size));
    LOG_DEBUG("End of RTSP message content");
	this->Send(data, size);
	return;
}

bool RtspConnection::onRead(BufferReader &buffer)
{


    return true;
}


std::shared_ptr<RtpTrack> createTrack(TrackType type, const std::string &codec_name, int payload_type, uint32_t clock_rate, int track_index)
{
     if (type == TrackType::TrackVideo) {
        return std::make_shared<RtpVideoTracker>(
            type, codec_name, payload_type, 0, clock_rate, track_index);
    }

    return std::make_shared<RtpAudioTracker>(
        type, codec_name, payload_type, 0, clock_rate, track_index);
}
