#include <iostream>
#include <cstring>      // memset
#include <sys/socket.h> // socket
#include <arpa/inet.h>  // sockaddr_in, inet_addr
#include <unistd.h>     // close

int main() {
    const char* server_ip = "127.0.0.1"; // 服务器IP
    const int server_port = 8888;        // 服务器端口

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) 
    {
        std::cerr << "Invalid address\n";
        close(sock);
        return 1;
    }

    
    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) 
    {
        std::cerr << "Connection failed\n";
        close(sock);
        return 1;
    }
    std::cout << "Connected to server\n";

    
    const char* message = "Hello from client!\n";
    send(sock, message, strlen(message), 0);

    // 接收数据
    char buffer[1024];
    int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0'; // null-terminate
        std::cout << "Received from server: " << buffer;
    }

    // 关闭连接
    close(sock);
    return 0;
}
