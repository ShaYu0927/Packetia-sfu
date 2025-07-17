#ifndef _UDPSERVER_H_
#define _UDPSERVER_H_

#include <stdint.h>
#include <mutex>
#include <memory>
#include <functional>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <stdexcept>
#include "Socket.h"

#include "BufferWrite.h"

class BufferRaw : public  BufferWirte{
public:
    BufferRaw(const char *data, size_t size) : _data(data, data + size) {}
    const char *data() const { return _data.data(); }
    size_t size() const  { return _data.size(); }
private:
    std::vector<char> _data;
};


class UDPServer : public std::enable_shared_from_this<UDPServer>
{
public:
    using onRecvData = std::function<bool(int intervaled, BufferWirte::Ptr& buffer, struct sockaddr *peer_addr)>;

    void listen(uint16_t port);
    void listenPeer(const std::string &peer_ip, void *obj, const onRecvData &cb);
    void stopListenPeer(const std::string &peer_ip, void *obj);
    void recvLoop();

private:
    void dispatchToCallbacks(const std::string &peer_ip, const std::vector<char> &data, sockaddr *addr);

    int _udp_fd = -1;
    std::atomic<bool> _running = false;
    std::thread _recv_thread;

    struct CallbackEntry {
        void *obj;
        onRecvData cb;
    };

    std::mutex _mtx;
    std::unordered_map<std::string, std::vector<CallbackEntry>> _callbacks;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _last_recv_time;
};


#endif