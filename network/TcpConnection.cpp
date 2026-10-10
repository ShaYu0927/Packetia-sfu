//
// Created by roots on 2024/9/11.
//

#include "TcpConnection.h"

TcpConnection::TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd)
    : TcpConnection(task_scheduler, sockfd, Options{})
{
}

TcpConnection::TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd, Options options)
    : read_buffer_(new BufferReader(2048, options.max_receive_bytes))
	, write_buffer_(new BufferWirte(options.max_send_bytes))
	, channel_(new Channel(sockfd))
    , task_scheduler_(task_scheduler)
    , scheduler_owner_(task_scheduler->weak_from_this().lock())
    , read_budget_bytes_(options.read_budget_bytes == 0 ? 128 * 1024 : options.read_budget_bytes)
{
    is_closed_ = false;

    SocketUtil::SetNonBlock(sockfd);
    SocketUtil::SetNoSigpipe(sockfd);
	SocketUtil::SetSendBufSize(sockfd, 100 * 1024);
	SocketUtil::SetKeepAlive(sockfd);
}

void TcpConnection::SetReadCallback(const ReadCallback& cb)
{
    task_scheduler_->Invoke([this, cb] { read_cb_ = cb; bytes_cb_ = {}; });
}

void TcpConnection::SetBytesCallback(BytesCallback cb)
{
    task_scheduler_->Invoke([this, cb = std::move(cb)] { bytes_cb_ = cb; read_cb_ = {}; });
}

void TcpConnection::SetCloseCallback(const CloseCallback& cb)
{
    task_scheduler_->Invoke([this, cb] { close_callback_ = cb; });
}

void TcpConnection::SetCloseCallback(SessionCloseCallback cb)
{
    task_scheduler_->Invoke([this, cb = std::move(cb)] { sess_close_cb_ = cb; });
}

void TcpConnection::SetDisconnectCallback(const DisconnectCallback& cb)
{
    task_scheduler_->Invoke([this, cb] { disconnect_callback_ = cb; });
}

void TcpConnection::SetWriteCompleteCallback(const WriteCompleteCallback& cb)
{
    task_scheduler_->Invoke([this, cb] { write_complete_callback_ = cb; });
}

size_t TcpConnection::SendCapacityBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return write_buffer_->CapacityBytes();
}

size_t TcpConnection::QueuedSendBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return write_buffer_->QueuedBytes();
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
            write_buffer_ = std::make_unique<BufferWirte>(write_buffer_->CapacityBytes());
            write_pending_ = false;
        }

        auto closed = close_callback_;
        auto session_closed = sess_close_cb_;
        auto disconnected = disconnect_callback_;
		if (closed)
        {
			closed(shared_from_this());
		}
        if (session_closed) session_closed(0);

		if (disconnected)
        {
			disconnected(shared_from_this());
		}	
        channel_->CloseSocket();
	}
}

void TcpConnection::HandleRead()
{
    if (is_closed_) return;
    bool peer_closed = false;
    uint32_t received = 0;

    while (received < read_budget_bytes_ &&
           read_buffer_->ReadableBytes() < read_buffer_->CapacityBytes())
    {
        int n = read_buffer_->Read(channel_->GetSocket(), read_budget_bytes_ - received);
        if (n > 0) { received += static_cast<uint32_t>(n); continue; }

        if (n == 0) { peer_closed = true; break; }

        if (errno == EINTR) continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) break;

        close();
        return;
    }

    
    if (bytes_cb_)
    {
        const size_t readable = read_buffer_->ReadableBytes();
        LOG_DEBUG("[TcpConnection] bytes_cb_ branch, fd=", GetSocket(), " readable=", readable);
        if (readable > 0)
        {
            auto p = reinterpret_cast<const uint8_t*>(read_buffer_->Peek());
            auto callback = bytes_cb_;
            callback(shared_from_this(), p, readable);
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

    if (!is_closed_ && !read_cb_ &&
        read_buffer_->ReadableBytes() >= read_buffer_->CapacityBytes())
    {
        close();
        return;
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

    auto callback = read_cb_;
    if (!callback(shared_from_this(), *read_buffer_))
    {
        close();
        return;
    }

    if (!is_closed_ && !read_continuation_pending_.load() &&
        read_buffer_->ReadableBytes() >= read_buffer_->CapacityBytes())
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
    const bool had_bytes = !write_buffer_->IsEmpty();

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
        auto callback = had_bytes ? write_complete_callback_ : WriteCompleteCallback{};
        lock.unlock();
        if (callback) callback(shared_from_this());
        if (peer_read_closed_ && !read_continuation_pending_.load()) FinishPeerRead();
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
