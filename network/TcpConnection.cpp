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
    // channel_->SetReadCallback([this]() { this->HandleRead(); });
	// channel_->SetWriteCallback([this]() { this->HandleWrite(); });
	// channel_->SetCloseCallback([this]() { this->HandleClose(); });
	// channel_->SetErrorCallback([this]() { this->HandleError(); });

    SocketUtil::SetNonBlock(sockfd);
	SocketUtil::SetSendBufSize(sockfd, 100 * 1024);
	SocketUtil::SetKeepAlive(sockfd);

    // channel_->EnableReading();
    // LOG_INFO("Channel EnableReading called. sockfd=" + std::to_string(sockfd));
    // task_scheduler_->UpdateChannel(channel_);
    // LOG_INFO("Channel updated in task scheduler. sockfd=" + std::to_string(sockfd));
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

void TcpConnection::Start()
{
    std::weak_ptr<TcpConnection> weak_self = shared_from_this();

    channel_->SetReadCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->HandleRead();
    });
    channel_->SetWriteCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->HandleWrite();
    });
    channel_->SetCloseCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->HandleClose();
    });
    channel_->SetErrorCallback([weak_self]() {
        if (auto self = weak_self.lock()) self->HandleError();
    });

    channel_->EnableReading();
    task_scheduler_->UpdateChannel(channel_);
    LOG_INFO("Channel registered. sockfd=" + std::to_string(channel_->GetSocket()));
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
    bool peer_closed = false;

    while (true) 
    {
        int n = read_buffer_->Read(channel_->GetSocket());

#if RTP_DEBUG
        LOG_INFO("Read returned: " + std::to_string(n));
#endif

        if (n > 0) 
        {
            continue; 
        }

        if (n == 0)
        {
            
            peer_closed = true;
            break;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) 
        {
            
            break;
        }

        LOG_ERROR("Read error, errno=" + std::to_string(errno));
        peer_closed = true;
        break;
    }

    
    if (read_cb_) 
    {
        read_cb_(shared_from_this(), *read_buffer_);
    }

    if (peer_closed) 
    {
        LOG_INFO("Peer closed connection (or fatal read error)");
        this->close();
        return;
    }

//     if (!parser_) 
//     {
//         parser_ = protocol_detector_->Detect(*read_buffer_);
//         if (!parser_) 
//         {
//             LOG_ERROR("Unknown protocol, closing connection");
//             this->close();
//             return;
//         }
//     }

//    while(true)
//    {
//         auto parse_result = parser_->Parse(*read_buffer_);
//         if (parse_result == ParseResult::Error) 
//         {
//             LOG_ERROR("Parse error, closing connection");
//             this->close();
//             return;
//         }

//         if(parse_result == ParseResult::NeedMoreData) 
//         {
//             return; // 需要更多数据，退出循环等待下一次读取
//         }
//         else if (parse_result == ParseResult::Ok)
//         {
//             // 成功解析一个完整包，继续循环解析剩余数据
//             continue;
//         }
//    }
    
}


void TcpConnection::HandleWrite()
{
    if (is_closed_) 
    {
        return;
    }
#if RTP_DEBUG
    LOG_INFO("HandleWrite called, write_buffer empty? " + std::to_string(write_buffer_->IsEmpty()));
#endif
	

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
