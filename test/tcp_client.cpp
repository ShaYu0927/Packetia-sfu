#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

void client_thread(int id, const char* server_ip, int server_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Client " << id << ": Socket creation failed\n";
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Client " << id << ": Invalid address\n";
        close(sock);
        return;
    }

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Client " << id << ": Connection failed\n";
        close(sock);
        return;
    }

    std::cout << "Client " << id << ": Connected to server\n";

    // 构造 RTSP OPTIONS 请求
    std::ostringstream oss;
    oss << "OPTIONS rtsp://127.0.0.1/test RTSP/1.0\r\n"
        << "CSeq: 1\r\n"
        << "User-Agent: RtspTestClient/1.0\r\n"
        << "\r\n";

    std::string request = oss.str();
    send(sock, request.c_str(), request.size(), 0);

    char buffer[4096] = {0};
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (received > 0) {
        std::cout << "Client " << id << ": Received response:\n";
        std::cout << buffer << std::endl;
    } else {
        std::cerr << "Client " << id << ": Failed to receive response\n";
    }

    close(sock);
    std::cout << "Client " << id << ": Connection closed\n";
}

int main() {
    const char* server_ip = "127.0.0.1";
    const int server_port = 554; // RTSP 默认端口
    const int num_clients = 3;

    std::vector<std::thread> clients;
    for (int i = 0; i < num_clients; ++i) {
        clients.emplace_back(client_thread, i, server_ip, server_port);
    }

    for (auto& t : clients) {
        t.join();
    }

    return 0;
}
