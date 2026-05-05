#include "websocket/WsServer.h"

namespace network 
{
namespace websocket 
{
class WsServer::Impl 
{
public:
    Impl() 
    {
        service_.onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
            OnOpen(channel, req);
        };

        service_.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
            OnMessage(channel, msg);
        };

        service_.onclose = [this](const WebSocketChannelPtr& channel) {
            OnClose(channel);
        };

        service_.setPingInterval(30000);
        server_.registerWebSocketService(&service_);
    }

    ~Impl() 
    {
        Stop();
    }

    bool Start(const std::string& ip, uint16_t port, int threadNum) 
    {
        if (started_.load()) 
        {
            return true;
        }
        server_.setHost(ip.c_str());
        server_.setPort(port);
        server_.setThreadNum(threadNum);

        int ret = server_.start();
        if (ret != 0) 
        {
            std::cerr << "[WsServer] start failed, ip="
                      << ip << ", port=" << port
                      << ", ret=" << ret << std::endl;
            return false;
        }

        started_.store(true);

        std::cout << "[WsServer] started, ip="
                  << ip << ", port=" << port
                  << ", threadNum=" << threadNum
                  << std::endl;

        return true;
    }

    void Stop() 
    {
        if (!started_.exchange(false)) 
        {
            return;
        }

        server_.stop();

        std::lock_guard<std::mutex> lock(mutex_);
        idToChannel_.clear();
        channelToId_.clear();

        std::cout << "[WsServer] stopped" << std::endl;
    }

    bool SendText(const std::string& connId, const std::string& message) 
    {
        WebSocketChannelPtr channel;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = idToChannel_.find(connId);
            if (it == idToChannel_.end()) 
            {
                return false;
            }

            channel = it->second;
        }

        if (!channel) 
        {
            return false;
        }

        int ret = channel->send(message);
        if (ret < 0) 
        {
            std::cerr << "[WsServer] send failed, connId=" << connId << ", ret=" << ret << std::endl;
            return false;
        }

        return true;
    }

    bool CloseConnection(const std::string& connId) 
    {
        WebSocketChannelPtr channel;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = idToChannel_.find(connId);
            if (it == idToChannel_.end()) 
            {
                return false;
            }

            channel = it->second;
        }

        if (!channel) 
        {
            return false;
        }

        channel->close();
        return true;
    }

    void SetOnOpen(OnOpenCallback cb) 
    {
        onOpen_ = std::move(cb);
    }

    void SetOnMessage(OnMessageCallback cb) 
    {
        onMessage_ = std::move(cb);
    }

    void SetOnClose(OnCloseCallback cb) 
    {
        onClose_ = std::move(cb);
    }

private:
    std::string CreateConnId() {
        uint64_t seq = connSeq_.fetch_add(1);
        std::ostringstream oss;
        oss << "WS_" << seq;
        return oss.str();
    }

    std::string GetConnId(const WebSocketChannelPtr& channel) 
    {
        if (!channel)
        {
            return "";
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = channelToId_.find(channel.get());
        if (it == channelToId_.end()) 
        {
            return "";
        }

        return it->second;
    }

    void OnOpen(const WebSocketChannelPtr& channel,const HttpRequestPtr& req) 
    {
        std::string connId = CreateConnId();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idToChannel_[connId] = channel;
            channelToId_[channel.get()] = connId;
        }

        WsConnectionInfo info;
        info.connId = connId;

        info.path = "";
        info.peerAddr = "";

        std::cout << "[WsServer] client connected, connId=" << connId << std::endl;

        if (onOpen_) 
        {
            onOpen_(info);
        }
    }

    void OnMessage(const WebSocketChannelPtr& channel, const std::string& message) 
    {
        std::string connId = GetConnId(channel);
        if (connId.empty()) 
        {
            return;
        }

        std::cout << "[WsServer] recv, connId=" << connId << ", message=" << message << std::endl;

        if (!onMessage_) 
        {
            return;
        }

        std::string response = onMessage_(connId, message);
        if (!response.empty()) 
        {
            SendText(connId, response);
        }
    }

    void OnClose(const WebSocketChannelPtr& channel) 
    {
        std::string connId;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = channelToId_.find(channel.get());
            if (it != channelToId_.end())
            {
                connId = it->second;

                idToChannel_.erase(connId);
                channelToId_.erase(it);
            }
        }

        std::cout << "[WsServer] client closed, connId=" << connId << std::endl;

        if (!connId.empty() && onClose_)
        {
            onClose_(connId);
        }
    }

private:
    hv::WebSocketService service_;
    hv::WebSocketServer server_;

    std::atomic_bool started_{false};
    std::atomic<uint64_t> connSeq_{0};

    std::mutex mutex_;

    std::unordered_map<std::string, WebSocketChannelPtr> idToChannel_;
    std::unordered_map<hv::WebSocketChannel*, std::string> channelToId_;
    OnOpenCallback onOpen_;
    OnMessageCallback onMessage_;
    OnCloseCallback onClose_;
};


WsServer::WsServer()
    : impl_(std::make_unique<Impl>()) 
{
}

WsServer::~WsServer() = default;

bool WsServer::Start(const std::string& ip, uint16_t port, int threadNum) 
{
    return impl_->Start(ip, port, threadNum);
}

void WsServer::Stop() 
{
    impl_->Stop();
}

bool WsServer::SendText(const std::string& connId, const std::string& message) 
{
    return impl_->SendText(connId, message);
}

bool WsServer::CloseConnection(const std::string& connId) 
{
    return impl_->CloseConnection(connId);
}

void WsServer::SetOnOpen(OnOpenCallback cb) 
{
    impl_->SetOnOpen(std::move(cb));
}

void WsServer::SetOnMessage(OnMessageCallback cb) 
{
    impl_->SetOnMessage(std::move(cb));
}

void WsServer::SetOnClose(OnCloseCallback cb) 
{
    impl_->SetOnClose(std::move(cb));
}



}
}