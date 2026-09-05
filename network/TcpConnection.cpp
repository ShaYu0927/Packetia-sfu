//
// Created by roots on 2024/9/11.
//

#include "TcpConnection.h"

TcpConnection::TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd)
    : task_scheduler_(task_scheduler)
    , scheduler_owner_(task_scheduler->weak_from_this().lock())
	, read_buffer_(new BufferReader)
	, write_buffer_(new BufferWirte())
	, channel_(new Channel(sockfd))
{
    is_closed_ = false;

    SocketUtil::SetNonBlock(sockfd);
	SocketUtil::SetSendBufSize(sockfd, 100 * 1024);
	SocketUtil::SetKeepAlive(sockfd);
}

TcpConnection::~TcpConnection()
{
    // No user callbacks from a destructor; remove the last registration
    // before destroying the channel.
    task_scheduler_->Invoke([this] {
        task_scheduler_->RemoveChannel(channel_);
        channel_->CloseSocket();
    });
}

void TcpConnection::Disconnect()
{
    close();
}

TcpConnection::SendResult TcpConnection::Send(std::shared_ptr<char> data, uint32_t size)
{
    if (!data || size == 0) return SendResult::Failed;
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_closed_ || task_scheduler_->IsStopped()) return SendResult::Closed;
    if (size > write_buffer_->CapacityBytes() - write_buffer_->QueuedBytes())
        return SendResult::QueueFull;
    if (!write_pending_)
    {
        auto weak = weak_from_this();
        if (weak.expired() || !task_scheduler_->Post([weak] {
                if (auto self = weak.lock()) self->HandleWrite();
            })) return SendResult::Failed;
        write_pending_ = true;
    }
    return write_buffer_->Append(std::move(data), size)
        ? SendResult::Queued : SendResult::QueueFull;
}

TcpConnection::SendResult TcpConnection::Send(const char *data, uint32_t size)
{
    if (!data || size == 0) return SendResult::Failed;
    // Check capacity before allocating/copying potentially oversized input.
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_closed_ || task_scheduler_->IsStopped()) return SendResult::Closed;
    if (size > write_buffer_->CapacityBytes() - write_buffer_->QueuedBytes())
        return SendResult::QueueFull;
    if (!write_pending_)
    {
        auto weak = weak_from_this();
        if (weak.expired() || !task_scheduler_->Post([weak] {
                if (auto self = weak.lock()) self->HandleWrite();
            })) return SendResult::Failed;
        write_pending_ = true;
    }
    return write_buffer_->Append(data, size)
        ? SendResult::Queued : SendResult::QueueFull;
}

void TcpConnection::Start()
{
    auto self = shared_from_this();
    task_scheduler_->Invoke([this, self] {
    if (started_ || is_closed_ || task_scheduler_->IsStopped()) return;
    started_ = true;
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
    });
}

void TcpConnection::close()
{
    auto self = shared_from_this();
    task_scheduler_->Invoke([self] { self->CloseOnOwner(); });
}

void TcpConnection::CloseOnOwner()
{
    if (!is_closed_.exchange(true))
    {
		task_scheduler_->RemoveChannel(channel_);
        // Preserve the descriptor identity for removal callbacks below.
        // Release the write lock before invoking any user callback.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            write_buffer_ = std::make_unique<BufferWirte>();
            write_pending_ = false;
        }

		if (close_callback_)
        {
			close_callback_(shared_from_this());
		}
        if (sess_close_cb_) sess_close_cb_(0);

		if (disconnect_callback_) 
        {
			disconnect_callback_(shared_from_this());
		}	
        channel_->CloseSocket();
	}
}

void TcpConnection::HandleRead()
{
    if (is_closed_) return;
    bool peer_closed = false;

    while (true)
    {
        int n = read_buffer_->Read(channel_->GetSocket());
        if (n > 0) continue;

        if (n == 0) { peer_closed = true; break; }

        if (errno == EINTR) continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) break;

        peer_closed = true;
        break;
    }

    
    if (bytes_cb_)
    {
        const size_t readable = read_buffer_->ReadableBytes();
        LOG_DEBUG("[TcpConnection] bytes_cb_ branch, fd=", GetSocket(), " readable=", readable);
        if (readable > 0)
        {
            auto p = reinterpret_cast<const uint8_t*>(read_buffer_->Peek());
            bytes_cb_(shared_from_this(), p, readable);
            read_buffer_->Retrieve(readable);
        }
    }
    else if (read_cb_)
    {
        if (peer_closed)
        {
            peer_read_closed_ = true;
        }
        const size_t readable = read_buffer_->ReadableBytes();
        LOG_DEBUG("[TcpConnection] read_cb_ branch, fd=", GetSocket(), " readable=", readable);
        DispatchReadCallback();
    }

    if (peer_closed && !read_cb_)
    {
        peer_read_closed_ = true;
        FinishPeerRead();
        return;
    }    
}

bool TcpConnection::RequestReadContinuation()
{
    if (is_closed_ || !task_scheduler_)
    {
        return false;
    }

    bool expected = false;
    if (!read_continuation_pending_.compare_exchange_strong(expected, true))
    {
        return true;
    }

    std::weak_ptr<TcpConnection> weak_self = shared_from_this();
    if (!task_scheduler_->Post([weak_self] {
            if (auto self = weak_self.lock())
            {
                self->read_continuation_pending_.store(false);
                self->DispatchReadCallback();
            }
        }))
    {
        read_continuation_pending_.store(false);
        return false;
    }
    return true;
}

void TcpConnection::DispatchReadCallback()
{
    if (is_closed_ || !read_cb_)
    {
        return;
    }

    if (!read_cb_(shared_from_this(), *read_buffer_))
    {
        close();
        return;
    }

    // If EOF arrived together with a large buffered batch, let budgeted
    // continuations drain it before closing the connection.
    if (peer_read_closed_ && !read_continuation_pending_.load())
    {
        FinishPeerRead();
    }
}

void TcpConnection::FinishPeerRead()
{
    if (is_closed_) return;
    bool drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained = write_buffer_->IsEmpty();
    }
    channel_->DisableReading();
    task_scheduler_->UpdateChannel(channel_);
    if (drained) close();
}

void TcpConnection::HandleWrite()
{
    if (is_closed_) 
    {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    write_pending_ = false;
    if (is_closed_) return;

    int ret = write_buffer_->Send(channel_->GetSocket());
    if (ret < 0) 
    {
        lock.unlock();
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
        if (peer_read_closed_ && !read_continuation_pending_.load())
        {
            lock.unlock();
            close();
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
	this->close();
}

void TcpConnection::HandleError()
{
    this->close();
}
