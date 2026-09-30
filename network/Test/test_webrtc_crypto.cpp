#include "DtlsTransport.h"
#include "SrtpTransport.h"

#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace protocol::webrtc;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace
{
uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Pair
{
    std::unique_ptr<DtlsTransport> client = CreateDtlsTransport();
    std::unique_ptr<DtlsTransport> server = CreateDtlsTransport();
    std::deque<std::vector<uint8_t>> toClient;
    std::deque<std::vector<uint8_t>> toServer;

    explicit Pair(bool wrongFingerprint = false)
    {
        CHECK(client && server);
        auto expectedServer = server->LocalParameters();
        CHECK(expectedServer.fingerprints.size() == 1);
        CHECK(expectedServer.fingerprints.front().value.size() == 95);
        if (wrongFingerprint)
        {
            auto& first = expectedServer.fingerprints.front().value.front();
            first = first == '0' ? '1' : '0';
        }
        CHECK(client->Configure(expectedServer, DtlsSetup::Active));
        CHECK(server->Configure(client->LocalParameters(), DtlsSetup::Passive));
        CHECK(server->Start([this](const uint8_t* data, size_t size)
        {
            toClient.emplace_back(data, data + size);
            return true;
        }));
        CHECK(client->Start([this](const uint8_t* data, size_t size)
        {
            toServer.emplace_back(data, data + size);
            return true;
        }));
    }

    bool Pump(bool dropFinalServerFlight = false)
    {
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            if (toClient.empty() && toServer.empty()) return true;
            if (!toServer.empty())
            {
                auto packet = std::move(toServer.front());
                toServer.pop_front();
                const bool wasConnected = server->IsConnected();
                if (!server->HandleDatagram(packet.data(), packet.size())) return false;
                if (dropFinalServerFlight && !wasConnected && server->IsConnected()) toClient.clear();
            }
            if (!toClient.empty())
            {
                auto packet = std::move(toClient.front());
                toClient.pop_front();
                if (!client->HandleDatagram(packet.data(), packet.size())) return false;
            }
        }
        throw std::runtime_error("DTLS exchange did not quiesce");
    }

    void CheckKeys(SrtpKeyingMaterial& clientKeys, SrtpKeyingMaterial& serverKeys)
    {
        CHECK(client->IsConnected() && server->IsConnected());
        CHECK(client->ExportSrtpKeys(clientKeys));
        CHECK(server->ExportSrtpKeys(serverKeys));
        CHECK(clientKeys.profile == 1 && serverKeys.profile == 1);
        CHECK(clientKeys.sendKey.size() == 30 && clientKeys.receiveKey.size() == 30);
        CHECK(clientKeys.sendKey == serverKeys.receiveKey);
        CHECK(clientKeys.receiveKey == serverKeys.sendKey);
        CHECK(clientKeys.sendKey != clientKeys.receiveKey);
    }
};

void CheckHandshakeAndFingerprint()
{
    Pair pair;
    SrtpKeyingMaterial clientKeys, serverKeys;
    CHECK(!pair.client->ExportSrtpKeys(clientKeys));
    CHECK(pair.Pump());
    pair.CheckKeys(clientKeys, serverKeys);
    pair.client->Close();
    pair.client->Close();
    CHECK(!pair.client->IsConnected());
    CHECK(!pair.client->ExportSrtpKeys(clientKeys));
    CHECK(!pair.client->Tick(NowMs()));

    Pair wrong(true);
    CHECK(!wrong.Pump());
    CHECK(!wrong.client->IsConnected());
    CHECK(!wrong.client->ExportSrtpKeys(clientKeys));

    auto invalid = CreateDtlsTransport();
    CHECK(invalid);
    auto remote = invalid->LocalParameters();
    remote.fingerprints.front().value = "00:11";
    CHECK(!invalid->Configure(remote, DtlsSetup::Active));
    remote = invalid->LocalParameters();
    remote.setup = DtlsSetup::Active;
    CHECK(!invalid->Configure(remote, DtlsSetup::Active));
}

void CheckRetransmissionAndTimeout()
{
    Pair dropped;
    CHECK(!dropped.toServer.empty());
    dropped.toServer.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(dropped.client->Tick(NowMs()));
    CHECK(!dropped.toServer.empty());
    CHECK(dropped.Pump());
    SrtpKeyingMaterial clientKeys, serverKeys;
    dropped.CheckKeys(clientKeys, serverKeys);

    // The server must still process retransmissions after it considers the
    // handshake complete, otherwise losing its final flight strands clients.
    Pair finalFlight;
    CHECK(finalFlight.Pump(true));
    CHECK(finalFlight.server->IsConnected());
    CHECK(!finalFlight.client->IsConnected());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(finalFlight.client->Tick(NowMs()));
    CHECK(finalFlight.server->Tick(NowMs()));
    CHECK(finalFlight.Pump());
    finalFlight.CheckKeys(clientKeys, serverKeys);

    Pair stalled;
    CHECK(!stalled.client->Tick(NowMs() + 16000));
    CHECK(!stalled.client->IsConnected());
    CHECK(!stalled.client->ExportSrtpKeys(clientKeys));
}

void CheckProtectedPackets(SrtpTransport& sender, SrtpTransport& receiver, bool rtcp, uint8_t sequence)
{
    const std::vector<uint8_t> plaintext = rtcp ?
        std::vector<uint8_t>{0x80, 201, 0, 1, 0x12, 0x34, 0x56, 0x78} :
        std::vector<uint8_t>{0x80, 111, 0, sequence, 0, 0, 1, 0x40, 0x12, 0x34, 0x56, 0x78, 1, 2, 3, 4};
    auto encrypted = plaintext;
    CHECK(rtcp ? sender.ProtectRtcp(encrypted) : sender.ProtectRtp(encrypted));
    CHECK(encrypted.size() == plaintext.size() + (rtcp ? 14u : 10u));
    CHECK(encrypted != plaintext);
    auto corrupted = encrypted;
    corrupted.back() ^= 1;
    CHECK(!(rtcp ? receiver.UnprotectRtcp(corrupted) : receiver.UnprotectRtp(corrupted)));
    CHECK(corrupted.empty());
    auto decrypted = encrypted;
    CHECK(rtcp ? receiver.UnprotectRtcp(decrypted) : receiver.UnprotectRtp(decrypted));
    CHECK(decrypted == plaintext);
    CHECK(!(rtcp ? receiver.UnprotectRtcp(encrypted) : receiver.UnprotectRtp(encrypted)));
    CHECK(encrypted.empty());
}

void CheckSrtp()
{
    Pair pair;
    CHECK(pair.Pump());
    SrtpKeyingMaterial clientKeys, serverKeys;
    pair.CheckKeys(clientKeys, serverKeys);
    auto client = CreateSrtpTransport();
    auto server = CreateSrtpTransport();
    CHECK(client && server);
    auto invalidKeys = clientKeys;
    invalidKeys.profile = 0;
    CHECK(!client->Configure(invalidKeys));
    CHECK(client->Configure(clientKeys));
    CHECK(server->Configure(serverKeys));
    CHECK(!client->Configure(clientKeys));
    CheckProtectedPackets(*client, *server, false, 1);
    CheckProtectedPackets(*server, *client, false, 1);
    CheckProtectedPackets(*client, *server, true, 0);
    CheckProtectedPackets(*server, *client, true, 0);
    std::vector<uint8_t> oversized(65508, 0);
    CHECK(!client->ProtectRtp(oversized));
    CHECK(oversized.empty());
    std::vector<uint8_t> malformed{0x80, 0, 0};
    CHECK(!server->UnprotectRtp(malformed));
    CHECK(malformed.empty());
    client->Close();
    client->Close();
    auto packet = std::vector<uint8_t>{0x80, 201, 0, 1, 0, 0, 0, 1};
    CHECK(!client->ProtectRtcp(packet));
    CHECK(packet.empty());
}
} // namespace

int main()
{
    try
    {
        CheckHandshakeAndFingerprint();
        CheckRetransmissionAndTimeout();
        CheckSrtp();
        std::cout << "WebRTC DTLS/SRTP tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WebRTC crypto test failed: " << error.what() << '\n';
        return 1;
    }
}
