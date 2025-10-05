//
// Created by roots on 2024/9/11.
//

#include "TcpConnection.h"

TcpConnection::TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd)
    : task_scheduler_(task_scheduler)
	, read_buffer_(new BufferReader)
	, write_buffer_(new BufferWirte(500))
	, channel_(new Channel(sockfd))
{
    is_closed_ = false;
    channel_->SetReadCallback([this]() { this->HandleRead(); });
	channel_->SetWriteCallback([this]() { this->HandleWrite(); });
	channel_->SetCloseCallback([this]() { this->HandleClose(); });
	channel_->SetErrorCallback([this]() { this->HandleError(); });

    SocketUtil::SetNonBlock(sockfd);
	SocketUtil::SetSendBufSize(sockfd, 100 * 1024);
	SocketUtil::SetKeepAlive(sockfd);

    channel_->EnableReading();
    LOG_INFO("Channel EnableReading called. sockfd=" + std::to_string(sockfd));
    task_scheduler_->UpdateChannel(channel_);
    LOG_INFO("Channel updated in task scheduler. sockfd=" + std::to_string(sockfd));
}

TcpConnection::~TcpConnection()
{
    SOCKET fd = channel_->GetSocket();
	if (fd > 0) {
		SocketUtil::Close(fd);
	}
}

void TcpConnection::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
	auto conn = shared_from_this();
	task_scheduler_->AddTriggerEvent([conn]() 
    {
		conn->close();
	});
}

void TcpConnection::Send(std::shared_ptr<char> data, uint32_t size)
{
    if (!is_closed_) 
    {
		mutex_.lock();
		write_buffer_->Append(data, size);
		mutex_.unlock();

		this->HandleWrite();
	}
}

void TcpConnection::Send(const char *data, uint32_t size)
{
    if (!is_closed_) 
    {
		mutex_.lock();
		write_buffer_->Append(data, size);
		mutex_.unlock();

		this->HandleWrite();
	}
}

void TcpConnection::close()
{
    if (!is_closed_) 
    {
		is_closed_ = true;
		task_scheduler_->RemoveChannel(channel_);

		if (close_callback_) 
        {
			close_callback_(shared_from_this());
		}			

		if (disconnect_callback_) 
        {
			disconnect_callback_(shared_from_this());
		}	
	}
}

void TcpConnection::HandleRead()
{
    // LOG_INFO("HandleRead: about to read from fd=" + std::to_string(channel_->GetSocket()));
    // {
    //     std::lock_guard<std::mutex> lock(mutex_);
    //     if (is_closed_) {
    //         LOG_INFO("HandleRead: connection already closed");
    //         return;
    //     }
        
    //     int ret = read_buffer_->Read(channel_->GetSocket());
    //     LOG_INFO("HandleRead: read_buffer_->Read returned " + std::to_string(ret));
    //     LOG_INFO("HandleRead: read_buffer_->Read returned " + std::to_string(ret));
    //     LOG_INFO("HandleRead: read_buffer_->ReadableBytes=" + std::to_string(read_buffer_->ReadableBytes()));
    //     if (ret <= 0) {
    //         LOG_INFO("HandleRead: read <= 0, closing connection");
    //         this->close();
    //         return;
    //     }
    // }
    // if (read_cb_) {
    //     bool ret = read_cb_(shared_from_this(), *read_buffer_);
    //     if (!ret) {
    //         std::lock_guard<std::mutex> lock(mutex_);
    //         LOG_INFO("HandleRead: read_cb_ returned false, closing connection");
    //         this->close();
    //     }
    // }

    int n = 0;
    do {
            n = read_buffer_->Read(channel_->GetSocket());
            LOG_INFO("Read returned: " + std::to_string(n));
    } while(n > 0);

    if (n == 0) {
        LOG_INFO("Peer closed connection");
        this->close();
        return;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("Read error");
            this->close();
            return;
        }

        // 调用 RTSP 解析函数
        if (read_cb_) read_cb_(shared_from_this(), *read_buffer_);
}


void TcpConnection::HandleWrite()
{
    if (is_closed_) {
        return;
    }
	LOG_INFO("HandleWrite called, write_buffer empty? " + std::to_string(write_buffer_->IsEmpty()));

    std::lock_guard<std::mutex> lock(mutex_);  // 一次锁定

    int ret = write_buffer_->Send(channel_->GetSocket());
    if (ret < 0) {
        this->close();
        return;
    }

    if (write_buffer_->IsEmpty()) 
    {
        if (channel_->IsWriting()) 
        {
            channel_->DisableWriting();
            task_scheduler_->UpdateChannel(channel_);
        }
    } 
    else 
    {
        if (!channel_->IsWriting()) 
        {
            channel_->EnableWriting();
            task_scheduler_->UpdateChannel(channel_);
        }
    }
}


void TcpConnection::HandleClose()
{
    std::lock_guard<std::mutex> lock(mutex_);
	this->close();
}

void TcpConnection::HandleError()
{
    std::lock_guard<std::mutex> lock(mutex_);
    this->close();
}
