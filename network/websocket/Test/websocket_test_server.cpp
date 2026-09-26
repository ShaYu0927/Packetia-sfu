#include "websocket/WsServer.h"
#include "websocket/WsSession.h"
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

void Require(bool result)
{
    if (!result) throw std::runtime_error("WebSocket session assertion failed");
}

void TestSession()
{
    int wakes = 0;
    auto session = std::make_shared<network::WsSession>("unit", [&] { ++wakes; });
    Require(!session->SendText("before open"));
    session->OnOpen();
    session->SetRoomId("room");
    session->SetParticipantId("participant");
    Require(session->IsJoinedRoom());
    int received = 0;
    session->SetOnMessage([&](const auto& id, const auto& message) {
        Require(id == "unit" && message == "hello");
        ++received;
        Require(session->SendText(message)); // reentrant, outside the lock
    });
    Require(session->ReceiveFragment("hel", 3, false));
    Require(received == 0);
    Require(session->ReceiveFragment("lo", 2, true));
    Require(received == 1 && wakes == 1);
    std::string result;
    Require(session->PopOutgoing(result) && result == "hello");
    Require(!session->PopOutgoing(result));
    for (std::size_t i = 0; i < network::WsSession::kMaxQueuedMessages; ++i)
        Require(session->SendText(""));
    Require(!session->SendText(""));
    while (session->PopOutgoing(result)) {}
    const std::string large(network::WsSession::kMaxMessageBytes, 'x');
    for (int i = 0; i < 4; ++i) Require(session->SendText(large));
    Require(!session->SendText("x"));
    Require(!session->SendText(large + "x"));
    while (session->PopOutgoing(result)) {}
    Require(session->ReceiveFragment(large.data(), large.size(), false));
    Require(!session->ReceiveFragment("x", 1, true));
    session->Close();
    Require(!session->SendText("closed"));
    session->OnClosed();
    Require(!session->IsJoinedRoom() && !session->NeedsWritable());

    auto concurrent = std::make_shared<network::WsSession>("concurrent", [] {});
    concurrent->OnOpen();
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i)
        writers.emplace_back([concurrent] {
            for (int n = 0; n < 50; ++n) Require(concurrent->SendText("queued"));
        });
    for (auto& writer : writers) writer.join();
    int count = 0;
    while (concurrent->PopOutgoing(result)) { Require(result == "queued"); ++count; }
    Require(count == 200);
    concurrent->OnClosed();
}

int main(int argc, char** argv)
{
    try
    {
        TestSession();
        if (argc != 2) return 2;
        const auto port = static_cast<uint16_t>(std::stoi(argv[1]));
        network::websocket::WsServer server;
        std::mutex output_mutex;
        auto emit = [&](const std::string& value) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << value << std::endl;
        };
        server.SetOnOpen([&](const auto& info) {
            emit("OPEN " + info.connId + " " + info.path + " " + info.peerAddr);
        });
        server.SetOnClose([&](const auto& id) { emit("CLOSED " + id); });
        server.SetOnMessage([&](const auto& id, const auto& message) -> std::string {
            if (message == "__close__") { server.CloseConnection(id); return {}; }
            if (message == "__stop__") { server.Stop(); return {}; }
            if (message == "__throw__") throw std::runtime_error("intentional callback failure");
            if (message == "__empty__") { server.SendText(id, ""); return {}; }
            return message;
        });
        Require(!server.Start("127.0.0.1", port, 2));
        if (!server.Start("127.0.0.1", port)) return 2;
        Require(server.Start("127.0.0.1", port)); // idempotent
        Require(!server.SendText("missing", "x"));
        Require(!server.CloseConnection("missing"));
        emit("READY");
        std::string command;
        while (std::getline(std::cin, command))
        {
            std::istringstream input(command);
            std::string op, id;
            input >> op >> id;
            if (op == "PUSH")
            {
                std::string message;
                std::getline(input >> std::ws, message);
                emit(server.SendText(id, message) ? "PUSHED" : "REJECTED");
            }
            else if (op == "CLOSE")
                emit(server.CloseConnection(id) ? "CLOSING" : "REJECTED");
            else if (op == "STOP")
            {
                server.Stop();
                server.Stop();
                emit("STOPPED");
            }
            else if (op == "START")
                emit(server.Start("127.0.0.1", port) ? "READY" : "FAILED");
            else if (op == "QUIT") break;
        }
        server.Stop();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
