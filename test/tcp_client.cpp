#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

void client_thread(int id, const char* server_ip, int server_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Client " << id << ": Socket creation failed\n";
        return;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
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

    const char* message = "Hello from client!\n";
    char buffer[1024];

    for (int i = 0; i < 10; ++i) { // 每个客户端发10次消息
        ssize_t sent_bytes = send(sock, message, strlen(message), 0);
        if (sent_bytes < 0) {
            std::cerr << "Client " << id << ": Send failed\n";
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    close(sock);
    std::cout << "Client " << id << ": Connection closed\n";
}

int main() {
    const char* server_ip = "127.0.0.1";
    const int server_port = 8888;
    const int num_clients = 5; // 启动 5 个客户端线程

    std::vector<std::thread> clients;
    for (int i = 0; i < num_clients; ++i) {
        clients.emplace_back(client_thread, i, server_ip, server_port);
    }

    for (auto& t : clients) {
        t.join();
    }

    return 0;
}


