//
// Created by roots on 2024/9/11.
//

#ifndef FFMPEGAAC_TCPCONNECTION_H
#define FFMPEGAAC_TCPCONNECTION_H

#include <memory>
#include <functional>
#include "TaskScheduler.h"
#include "BufferWrite.h"
#include "BufferRead.h"
#include "SocketUtil.h"


class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
public:

    enum class ConnectionType { Generic, Rtsp };
    virtual ConnectionType GetConnectionType() const { return ConnectionType::Generic; }

    using Ptr = std::shared_ptr<TcpConnection>;
    using DisconnectCallback = std::function<void(std::shared_ptr<TcpConnection> conn)>;
    using MessageCallback = std::function<void(std::shared_ptr<TcpConnection> conn, const char* data, int len)>;
    using ReadCallback = std::function<bool(std::shared_ptr<TcpConnection> conn,BufferReader& buffer)>;
    using WriteCompleteCallback = std::function<void(std::shared_ptr<TcpConnection> conn)>;
    using CloseCallback = std::function<void(std::shared_ptr<TcpConnection> conn)>;
    using ErrorCallback = std::function<void(std::shared_ptr<TcpConnection> conn)>;
    using ConnectionCallback = std::function<void(std::shared_ptr<TcpConnection> conn)>;
    using HighWaterMarkCallback = std::function<void(std::shared_ptr<TcpConnection> conn, size_t len)>;

    using BytesCallback = std::function<void(std::shared_ptr<TcpConnection>,
                                        const uint8_t*, size_t)>;
    using SessionCloseCallback = std::function<void(int reason)>;


    TcpConnection(TaskScheduler *task_scheduler, SOCKET sockfd);
    virtual ~TcpConnection();

    TaskScheduler* GetTaskScheduler() const 
	{ return task_scheduler_; }


    void SetReadCallback(const ReadCallback& cb)
	{ read_cb_ = cb; }

    void SetCloseCallback(const CloseCallback& cb)
	{ close_callback_ = cb; }

    void SetDisconnectCallback(const DisconnectCallback& cb)
	{ disconnect_callback_ = cb; }


    void SetBytesCallback(BytesCallback cb) { bytes_cb_ = std::move(cb); }
    void SetCloseCallback(SessionCloseCallback cb) { sess_close_cb_ = std::move(cb); }

    void Disconnect();
    void Send(std::shared_ptr<char> data, uint32_t size);
	void Send(const char *data, uint32_t size);
    void Start();


    bool IsClosed() const 
	{ return is_closed_; }

    SOCKET GetSocket() const
	{ return channel_->GetSocket(); }

    uint16_t GetPort() const
	{ return SocketUtil::GetPeerPort(channel_->GetSocket()); }
    
	std::string GetIp() const
	{ return SocketUtil::GetPeerIp(channel_->GetSocket()); }


    void close();
protected:
    virtual void HandleRead();
	virtual void HandleWrite();
	virtual void HandleClose();
	virtual void HandleError();	


    std::unique_ptr<BufferReader> read_buffer_;
	std::unique_ptr<BufferWirte>  write_buffer_;

    DisconnectCallback disconnect_callback_;
    MessageCallback message_callback_;
    ReadCallback read_cb_;
    WriteCompleteCallback write_complete_callback_;
    CloseCallback close_callback_;
    ErrorCallback error_callback_;
    ConnectionCallback connection_callback_;
    HighWaterMarkCallback high_water_mark_callback_;


    BytesCallback bytes_cb_;
    SessionCloseCallback sess_close_cb_;

    std::shared_ptr<Channel> channel_;
    TaskScheduler *task_scheduler_;
    std::mutex mutex_;
    std::atomic_bool is_closed_;

};


#endif //FFMPEGAAC_TCPCONNECTION_H
