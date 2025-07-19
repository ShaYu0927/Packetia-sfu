#ifndef _UDPSERVER_H_
#define _UDPSERVER_H_

#include <functional>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <memory>
#include <algorithm>
#include <netinet/in.h>  // for sockaddr_in, sockaddr, etc.
#include <sys/socket.h>
#include <arpa/inet.h>   // for inet_ntoa

// 类型定义
using onRecvData = std::function<void(int interval, const std::shared_ptr<std::vector<char>> &data, sockaddr *addr)>;
using CallbackEntry = std::pair<void*, onRecvData>;

class UDPServer {
public:
    UDPServer() : _udp_fd(-1), _running(false) {}

    void listen(uint16_t port);
    void listenPeer(const std::string &peer_ip, void *obj, const onRecvData &cb);
    void stopListenPeer(const std::string &peer_ip, void *obj);
    void stop();

private:
    void recvLoop();
    void dispatchToCallbacks(const std::string &peer_ip, const std::vector<char> &data, sockaddr *addr);

    int _udp_fd;
    bool _running;
    std::thread _recv_thread;
    std::mutex _mtx;
    std::map<std::string, std::vector<CallbackEntry>> _callbacks; // 绑定IP与回调的映射
    std::map<std::string, std::chrono::steady_clock::time_point> _last_recv_time; // 记录每个peer的最后接收时间
};


#endif