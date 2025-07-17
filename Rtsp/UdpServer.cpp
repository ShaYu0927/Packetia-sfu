#include "UdpServer.h"

void UDPServer::listen(uint16_t port)
{
    if (_udp_fd > 0) return;

    _udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (_udp_fd <= 0) throw std::runtime_error("Failed to create UDP socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(_udp_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        throw std::runtime_error("Bind failed");
    }

    _running = true;
    _recv_thread = std::thread(&UDPServer::recvLoop, this);
}

void UDPServer::listenPeer(const std::string &peer_ip, void *obj, const onRecvData &cb)
{
    std::lock_guard<std::mutex> lck(_mtx);
    auto &vec = _callbacks[peer_ip];
    for (auto &entry : vec) {
        if (entry.obj == obj) return; // 已存在
    }
    vec.push_back({obj, cb});
}

void UDPServer::stopListenPeer(const std::string &peer_ip, void *obj)
{
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _callbacks.find(peer_ip);
    if (it != _callbacks.end()) {
        auto &vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [obj](const CallbackEntry &e) { return e.obj == obj; }),
                  vec.end());
        if (vec.empty()) _callbacks.erase(it);
    }
}

void UDPServer::recvLoop()
{
    char buffer[1500];
    while (_running) {
        sockaddr_in peer_addr{};
        socklen_t addr_len = sizeof(peer_addr);
        int n = ::recvfrom(_udp_fd, buffer, sizeof(buffer), 0,
                           (sockaddr*)&peer_addr, &addr_len);
        if (n <= 0) continue;

        std::string peer_ip = inet_ntoa(peer_addr.sin_addr);
        std::vector<char> data(buffer, buffer + n);
        dispatchToCallbacks(peer_ip, data, (sockaddr*)&peer_addr);
    }
}

void UDPServer::dispatchToCallbacks(const std::string &peer_ip, const std::vector<char> &data, sockaddr *addr)
{
    std::vector<CallbackEntry> cbs;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _callbacks.find(peer_ip);
        if (it == _callbacks.end()) return;
        cbs = it->second;
    }

    // 计算 interval
    int intervaled = 0;
    auto now = std::chrono::steady_clock::now();
    auto &last = _last_recv_time[peer_ip];
    if (last.time_since_epoch().count() != 0) {
        intervaled = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    }
    last = now;

    // 创建 Buffer::Ptr
    auto buffer = std::make_shared<BufferWirte>(data.data(), data.size());

    for (const auto &entry : cbs) {
        entry.cb(intervaled, buffer, addr);
    }
}
