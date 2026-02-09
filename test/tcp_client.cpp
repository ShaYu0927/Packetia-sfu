#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>

class RtspClient 
{
public:
    RtspClient(const char* ip, int port)
        : server_ip(ip), server_port(port), cseq(1), session_id("") {}

    bool connect_server() 
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) 
        {
            std::cerr << "Socket creation failed\n";
            return false;
        }
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) 
        {
            std::cerr << "Invalid address\n";
            close(sock);
            return false;
        }
        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) 
        {
            std::cerr << "Connection failed\n";
            close(sock);
            return false;
        }
        return true;
    }

    bool send_request(const std::string& request) 
    {
        ssize_t sent = send(sock, request.c_str(), request.size(), 0);
        return sent == (ssize_t)request.size();
    }

    bool receive_response(std::string& response) 
    {
        char buffer[4096] = {0};
        ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) 
        {
            response.assign(buffer, received);
            return true;
        }
        return false;
    }

    void close_connection() 
    {
        if (sock >= 0) close(sock);
        sock = -1;
    }

    void run(const std::string& rtsp_url) 
    {
        if (!connect_server()) return;

        // 1. OPTIONS
        std::string options_req = build_options(rtsp_url);
        std::cout << "Sending OPTIONS:\n" << options_req << std::endl;
        send_request(options_req);
        std::string response;
        if (receive_response(response)) 
        {
            std::cout << "Received OPTIONS response:\n" << response << std::endl;
        } 
        else 
        {
            std::cerr << "Failed to receive OPTIONS response\n";
            close_connection();
            return;
        }

        // 2. DESCRIBE
        std::string describe_req = build_describe(rtsp_url);
        std::cout << "Sending DESCRIBE:\n" << describe_req << std::endl;
        send_request(describe_req);
        if (receive_response(response)) 
        {
            std::cout << "Received DESCRIBE response:\n" << response << std::endl;
        } 
        else 
        {
            std::cerr << "Failed to receive DESCRIBE response\n";
            close_connection();
            return;
        }

        // 3. SETUP
        std::string setup_req = build_setup(rtsp_url);
        std::cout << "Sending SETUP:\n" << setup_req << std::endl;
        send_request(setup_req);
        if (receive_response(response)) 
        {
            std::cout << "Received SETUP response:\n" << response << std::endl;
            // 解析 Session ID
            parse_session(response);
        } 
        else 
        {
            std::cerr << "Failed to receive SETUP response\n";
            close_connection();
            return;
        }

        // 4. PLAY
        std::string play_req = build_play(rtsp_url);
        std::cout << "Sending PLAY:\n" << play_req << std::endl;
        send_request(play_req);
        if (receive_response(response)) 
        {
            std::cout << "Received PLAY response:\n" << response << std::endl;
        } 
        else 
        {
            std::cerr << "Failed to receive PLAY response\n";
        }

        close_connection();
    }

private:
    const char* server_ip;
    int server_port;
    int sock{-1};
    int cseq;
    std::string session_id;

    std::string build_options(const std::string& url) {
        std::ostringstream oss;
        oss << "OPTIONS " << url << " RTSP/1.0\r\n"
            << "CSeq: " << cseq++ << "\r\n"
            << "User-Agent: RtspTestClient/1.0\r\n"
            << "\r\n";
        return oss.str();
    }

    std::string build_describe(const std::string& url) {
        std::ostringstream oss;
        oss << "DESCRIBE " << url << " RTSP/1.0\r\n"
            << "CSeq: " << cseq++ << "\r\n"
            << "Accept: application/sdp\r\n"
            << "User-Agent: RtspTestClient/1.0\r\n"
            << "\r\n";
        return oss.str();
    }

    std::string build_setup(const std::string& url) {
        std::ostringstream oss;
        // 假设请求第一个 track (trackID=0)，可根据服务器实际调整
        oss << "SETUP " << url << "/trackID=0 RTSP/1.0\r\n"
            << "CSeq: " << cseq++ << "\r\n"
            << "Transport: RTP/AVP;unicast;client_port=8000-8001\r\n"
            << "User-Agent: RtspTestClient/1.0\r\n"
            << "\r\n";
        return oss.str();
    }

    std::string build_play(const std::string& url) {
        std::ostringstream oss;
        oss << "PLAY " << url << " RTSP/1.0\r\n"
            << "CSeq: " << cseq++ << "\r\n";
        if (!session_id.empty()) {
            oss << "Session: " << session_id << "\r\n";
        }
        oss << "User-Agent: RtspTestClient/1.0\r\n"
            << "\r\n";
        return oss.str();
    }

    void parse_session(const std::string& response) {
        // 简单查找 Session: 字段并提取
        size_t pos = response.find("Session:");
        if (pos != std::string::npos) {
            size_t start = pos + 8;
            size_t end = response.find("\r\n", start);
            if (end != std::string::npos) {
                session_id = response.substr(start, end - start);
                // 去除前后空白
                session_id.erase(0, session_id.find_first_not_of(" \t"));
                session_id.erase(session_id.find_last_not_of(" \t") + 1);
                std::cout << "Parsed Session ID: " << session_id << std::endl;
            }
        }
    }
};

int main() {
    const char* server_ip = "127.0.0.1";
    const int server_port = 554;
    std::string rtsp_url = "rtsp://127.0.0.1/test";

    RtspClient client(server_ip, server_port);
    client.run(rtsp_url);

    return 0;
}
