//
// Created by roots on 2024/9/11.
//

#include "TcpConnection.h"

TcpConnection::TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd)
    : task_scheduler_(task_scheduler)
	// , read_buffer_(new BufferReader)
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
	task_scheduler_->UpdateChannel(channel_);
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
    
}

void TcpConnection::HandleWrite()
{
    if (is_closed_) {
		return;
	}
	
	//std::lock_guard<std::mutex> lock(mutex_);
	if (!mutex_.try_lock()) {
		return;
	}

	int ret = 0;
	bool empty = false;
	do
	{
		ret = write_buffer_->Send(channel_->GetSocket());
		if (ret < 0) 
        {
			this->close();
			mutex_.unlock();
			return;
		}
		empty = write_buffer_->IsEmpty();
	} while (0);

	if (empty) {
		if (channel_->IsWriting()) {
			channel_->DisableWriting();
			task_scheduler_->UpdateChannel(channel_);
		}
	}
	else if(!channel_->IsWriting()) {
		channel_->EnableWriting();
		task_scheduler_->UpdateChannel(channel_);
	}

	mutex_.unlock();
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
