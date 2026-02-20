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

    SocketUtil::SetNonBlock(sockfd);
	SocketUtil::SetSendBufSize(sockfd, 100 * 1024);
	SocketUtil::SetKeepAlive(sockfd);
}

TcpConnection::~TcpConnection()
{
    SOCKET fd = channel_->GetSocket();
	if (fd > 0)
    {
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
    int  fatal_errno = 0;

    while (true)
    {
        int n = read_buffer_->Read(channel_->GetSocket());
        if (n > 0) continue;

        if (n == 0) { peer_closed = true; break; }

        if (errno == EAGAIN || errno == EWOULDBLOCK) break;

        fatal_errno = errno;
        peer_closed = true;
        break;
    }

    
    if (bytes_cb_)
    {
        LOG_DEBUG("[TcpConnection] bytes_cb_ branch, fd=",
              GetSocket(),
              " readable=", n);
        size_t n = read_buffer_->ReadableBytes();
        if (n > 0)
        {
            auto p = reinterpret_cast<const uint8_t*>(read_buffer_->Peek());
            bytes_cb_(shared_from_this(), p, n);
            read_buffer_->Retrieve(n); 
        }
    }
    else if (read_cb_)
    {
        LOG_DEBUG("[TcpConnection] read_cb_ branch, fd=",
              GetSocket(),
              " readable=", n);
        read_cb_(shared_from_this(), *read_buffer_);
    }

    if (peer_closed) 
    {
        this->close();
        return;
    }    
}


void TcpConnection::HandleWrite()
{
    if (is_closed_) 
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);  

    int ret = write_buffer_->Send(channel_->GetSocket());
    if (ret < 0) 
    {
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
