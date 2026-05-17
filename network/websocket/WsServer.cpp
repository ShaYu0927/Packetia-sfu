#include "websocket/WsServer.h"
#include "WsSession.h"
#include "WsSessionManager.h"
#include <memory>

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

        session_manager_ = std::make_shared<WsSessionManager>();
    }

    ~Impl() { Stop(); }

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
            std::cerr << "[WsServer] start failed, ip=" << ip << ", port=" << port << ", ret=" << ret << std::endl;
            return false;
        }

        started_.store(true);

        std::cout << "[WsServer] started, ip=" << ip << ", port=" << port << ", threadNum=" << threadNum << std::endl;

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
        channelToId_.clear();

        std::cout << "[WsServer] stopped" << std::endl;
    }


    bool CloseConnection(const std::string& connId) 
    {
        WebSocketChannelPtr channel;
        return true;
    }

    void SetOnOpen(OnOpenCallback cb) { onOpen_ = std::move(cb);}

    void SetOnMessage(OnMessageCallback cb) { onMessage_ = std::move(cb); }

    void SetOnClose(OnCloseCallback cb) { onClose_ = std::move(cb); }

private:
    std::string CreateConnId() 
    {
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
        auto session = std::make_shared<WsSession>(connId, channel);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            channelToId_[channel.get()] = connId;
        }

        if (!session_manager_->AddSession(session))
        {
            channel->close();
            return;
        }


        session->OnOpen();
    }

    void OnMessage(const WebSocketChannelPtr& channel, const std::string& message) 
    {
        WsSession::Ptr session;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = channelToId_.find(channel.get());
            if (it == channelToId_.end()) 
            {
                return;
            }
            auto session = session_manager_->GetSession(it->second);
            if (!session)
            {
                return;
            }

        }

        session->OnMessage(message);
    }

    void OnClose(const WebSocketChannelPtr& channel) 
    {

    }

private:
    hv::WebSocketService service_;
    hv::WebSocketServer server_;

    std::atomic_bool started_{false};
    std::atomic<uint64_t> connSeq_{0};

    std::mutex mutex_;

    std::unordered_map<hv::WebSocketChannel*, std::string> channelToId_;
    OnOpenCallback onOpen_;
    OnMessageCallback onMessage_;
    OnCloseCallback onClose_;

    std::shared_ptr<WsSessionManager> session_manager_;
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