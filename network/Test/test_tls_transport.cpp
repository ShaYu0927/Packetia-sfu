#include "transport/TlsTransport.h"
#include "EventLoop.h"

#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using network::transport::TlsContext;
using network::transport::TlsTransport;

void Require(bool value, const char* reason)
{
    if (!value) throw std::runtime_error(reason);
}

struct CertificateFiles
{
    std::filesystem::path directory;
    std::string certificate;
    std::string key;

    CertificateFiles()
    {
        static std::atomic<unsigned> counter{0};
        directory = std::filesystem::temp_directory_path() /
            ("packetia-tls-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(counter++));
        std::filesystem::create_directories(directory);
        certificate = (directory / "certificate.pem").string();
        key = (directory / "key.pem").string();
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        Require(generator && EVP_PKEY_keygen_init(generator.get()) == 1 &&
                    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) == 1,
                "EC key generator");
        EVP_PKEY* generated = nullptr;
        Require(EVP_PKEY_keygen(generator.get(), &generated) == 1, "EC key generation");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key(generated, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        Require(cert && X509_set_version(cert.get(), 2) == 1 &&
                    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1 &&
                    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) &&
                    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) &&
                    X509_set_pubkey(cert.get(), private_key.get()) == 1, "test certificate");
        auto* name = X509_get_subject_name(cert.get());
        Require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                    reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1 &&
                    X509_set_issuer_name(cert.get(), name) == 1, "test certificate identity");
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                               const_cast<char*>("DNS:localhost")), X509_EXTENSION_free);
        Require(san && X509_add_ext(cert.get(), san.get(), -1) == 1 &&
                    X509_sign(cert.get(), private_key.get(), EVP_sha256()) > 0, "certificate signing");
        std::unique_ptr<FILE, decltype(&std::fclose)> cert_file(std::fopen(certificate.c_str(), "wb"), std::fclose);
        std::unique_ptr<FILE, decltype(&std::fclose)> key_file(std::fopen(key.c_str(), "wb"), std::fclose);
        Require(cert_file && key_file && PEM_write_X509(cert_file.get(), cert.get()) == 1 &&
                    PEM_write_PrivateKey(key_file.get(), private_key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
                "certificate files");
    }
    ~CertificateFiles() { std::filesystem::remove_all(directory); }
    TlsContext::Options Options() const { return {certificate, key}; }
};

// Real OpenSSL client driven through a socket pair. Small writes deliberately
// fragment TLS records independently of TCP read boundaries.
class Client
{
public:
    explicit Client(int fd, const CertificateFiles& files, int version, bool trust = true)
        : fd_(fd), context_(SSL_CTX_new(TLS_client_method()), SSL_CTX_free), ssl_(nullptr, SSL_free)
    {
        Require(context_ && SSL_CTX_set_min_proto_version(context_.get(), version) == 1 &&
                    SSL_CTX_set_max_proto_version(context_.get(), version) == 1, "client TLS version");
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        if (trust) Require(SSL_CTX_load_verify_locations(context_.get(), files.certificate.c_str(), nullptr) == 1,
                           "client trust store");
        ssl_.reset(SSL_new(context_.get()));
        input_ = BIO_new(BIO_s_mem());
        output_ = BIO_new(BIO_s_mem());
        Require(ssl_ && input_ && output_, "client TLS buffers");
        BIO_set_mem_eof_return(input_, -1);
        SSL_set_bio(ssl_.get(), input_, output_);
        SSL_set_connect_state(ssl_.get());
        Require(SSL_set1_host(ssl_.get(), "localhost") == 1, "client hostname validation");
        SocketUtil::SetNoSigpipe(fd_);
    }
    ~Client() { if (fd_ >= 0) ::close(fd_); }

    void Tick()
    {
        Flush();
        std::array<uint8_t, 16 * 1024> bytes{};
        for (;;) {
            const auto read = ::recv(fd_, bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (read <= 0) break;
            Require(BIO_write(input_, bytes.data(), static_cast<int>(read)) == read, "client input");
        }
        if (failed || closed) return;
        ERR_clear_error();
        if (!open) {
            const int result = SSL_do_handshake(ssl_.get());
            const int code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_.get(), result);
            open = result == 1;
            failed = result != 1 && code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE;
        }
        if (open) {
            for (;;) {
                size_t size = 0;
                ERR_clear_error();
                const int result = SSL_read_ex(ssl_.get(), bytes.data(), bytes.size(), &size);
                const int code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_.get(), result);
                if (result == 1) { received.insert(received.end(), bytes.begin(), bytes.begin() + size); continue; }
                closed = code == SSL_ERROR_ZERO_RETURN;
                failed = !closed && code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE;
                break;
            }
        }
        Flush();
    }

    void Write(const std::vector<uint8_t>& bytes)
    {
        size_t offset = 0;
        while (offset < bytes.size()) {
            size_t size = 0;
            Require(SSL_write_ex(ssl_.get(), bytes.data() + offset,
                                std::min<size_t>(16 * 1024, bytes.size() - offset), &size) == 1,
                    "client TLS write");
            offset += size;
            Flush();
        }
    }

    void ShutdownTls() { SSL_shutdown(ssl_.get()); Flush(); }
    void DropTcp() { ::close(fd_); fd_ = -1; }
    int Fd() const { return fd_; }
    size_t fragment_bytes = 16 * 1024;
    bool open = false;
    bool closed = false;
    bool failed = false;
    std::vector<uint8_t> received;

private:
    void Flush()
    {
        if (fd_ < 0) return;
        std::array<uint8_t, 16 * 1024> discard{};
        while (BIO_ctrl_pending(output_) > 0) {
            char* data = nullptr;
            const auto available = BIO_get_mem_data(output_, &data);
            const auto size = std::min({static_cast<size_t>(available), fragment_bytes, discard.size()});
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
            constexpr int flags = MSG_DONTWAIT;
#endif
            const auto written = ::send(fd_, data, size, flags);
            if (written <= 0) break;
            Require(BIO_read(output_, discard.data(), static_cast<int>(written)) == written, "client output");
        }
    }
    int fd_;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_;
    BIO* input_ = nullptr;
    BIO* output_ = nullptr;
};

struct Harness
{
    CertificateFiles files;
    EventLoop loop{1};
    std::shared_ptr<TaskScheduler> scheduler;
    TcpConnection::Ptr tcp;
    std::shared_ptr<TlsTransport> tls;
    std::unique_ptr<Client> client;
    std::atomic<int> ready{0};
    std::atomic<int> closed{0};
    std::atomic<bool> echo{false};
    std::atomic<bool> send_failed{false};
    std::atomic<TlsTransport::Error> last_error{TlsTransport::Error::None};
    std::mutex mutex;
    std::vector<uint8_t> received;

    Harness(TlsTransport::Options options = {}, size_t tcp_capacity = 4 * 1024 * 1024,
            int version = TLS1_2_VERSION, bool trust = true)
    {
        Require(loop.Start(), "I/O loop");
        scheduler = loop.GetTaskScheduler();
        int sockets[2]{};
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socket pair");
        TcpConnection::Options tcp_options;
        tcp_options.max_send_bytes = tcp_capacity;
        tcp = std::make_shared<TcpConnection>(scheduler.get(), sockets[0], tcp_options);
        auto context = TlsContext::CreateServer(files.Options());
        Require(static_cast<bool>(context), "server context");
        tls = std::make_shared<TlsTransport>(tcp, std::move(context), options);
        client = std::make_unique<Client>(sockets[1], files, version, trust);
        Require(tls->Start({[this] { ++ready; },
            [this](const uint8_t* data, size_t size) {
                { std::lock_guard<std::mutex> lock(mutex); received.insert(received.end(), data, data + size); }
                if (echo && tls->Send(data, size) != TlsTransport::SendResult::Queued) send_failed = true;
            },
            [this](TlsTransport::Error reason) { last_error = reason; ++closed; }}), "TLS start");
    }
    ~Harness()
    {
        if (tls) tls->Close();
        tls.reset();
        tcp.reset();
        loop.Stop();
    }

    bool Wait(const std::function<bool()>& condition, bool pump_client = true)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pump_client) client->Tick();
            if (condition()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
    bool Handshake() { return Wait([this] { return client->open && ready == 1; }); }
};

TEST(TlsContextTest, ValidatesCertificateFilesAndMatchingPrivateKey)
{
    std::string error;
    EXPECT_FALSE(TlsContext::CreateServer({}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(TlsContext::CreateServer({"/missing/certificate", "/missing/key"}, &error));
    CertificateFiles first;
    CertificateFiles second;
    EXPECT_FALSE(TlsContext::CreateServer({first.certificate, second.key}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(TlsContext::CreateServer(first.Options(), &error));
    EXPECT_TRUE(error.empty());
}

class TlsVersionTest : public testing::TestWithParam<int> {};

TEST_P(TlsVersionTest, FragmentedHandshakeAndBinaryEcho)
{
    Harness h({}, 4 * 1024 * 1024, GetParam());
    h.client->fragment_bytes = 7;
    h.echo = true;
    EXPECT_EQ(h.tls->Send(reinterpret_cast<const uint8_t*>("x"), 1), TlsTransport::SendResult::NotWritable);
    ASSERT_TRUE(h.Handshake());
    ASSERT_EQ(h.tls->GetState(), TlsTransport::State::Open);
    std::vector<uint8_t> payload(64 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i % 251);
    h.client->fragment_bytes = 1024;
    h.client->Write(payload);
    ASSERT_TRUE(h.Wait([&] { return h.client->received.size() == payload.size(); }));
    EXPECT_EQ(h.client->received, payload);
    { std::lock_guard<std::mutex> lock(h.mutex); EXPECT_EQ(h.received, payload); }
    EXPECT_FALSE(h.send_failed);
    EXPECT_EQ(h.ready, 1);
    EXPECT_EQ(h.closed, 0);
}

INSTANTIATE_TEST_SUITE_P(TlsVersions, TlsVersionTest,
                        testing::Values(TLS1_2_VERSION, TLS1_3_VERSION));

TEST(TlsTransportTest, TinyTcpQueueRetriesCiphertextWithoutLossOrReordering)
{
    TlsTransport::Options options;
    options.max_plaintext_bytes = 256 * 1024;
    Harness h(options, 128);
    ASSERT_TRUE(h.Handshake());
    std::vector<uint8_t> payload(options.max_plaintext_bytes);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i % 251);
    EXPECT_EQ(h.tls->Send(payload.data(), payload.size()), TlsTransport::SendResult::Queued);
    const std::vector<uint8_t> oversized(payload.size() + 1);
    EXPECT_EQ(h.tls->Send(oversized.data(), oversized.size()), TlsTransport::SendResult::NotWritable);
    ASSERT_TRUE(h.Wait([&] { return h.client->received.size() == payload.size(); }));
    EXPECT_EQ(h.client->received, payload);
    EXPECT_EQ(h.closed, 0);
    EXPECT_EQ(h.tls->Send(reinterpret_cast<const uint8_t*>("end"), 3), TlsTransport::SendResult::Queued);
    ASSERT_TRUE(h.Wait([&] { return h.client->received.size() == payload.size() + 3; }));
    EXPECT_EQ(std::string(h.client->received.end() - 3, h.client->received.end()), "end");
}

TEST(TlsTransportTest, LargeOutputContinuesAfterPerRoundBudget)
{
    Harness h;
    ASSERT_TRUE(h.Handshake());
    const std::vector<uint8_t> payload(768 * 1024, 0xA5);
    EXPECT_EQ(h.tls->Send(payload.data(), payload.size()), TlsTransport::SendResult::Queued);
    ASSERT_TRUE(h.Wait([&] { return h.client->received.size() == payload.size(); }));
    EXPECT_EQ(h.client->received, payload);
    EXPECT_EQ(h.closed, 0);
}

TEST(TlsTransportTest, HandshakeTimeoutClosesExactlyOnce)
{
    TlsTransport::Options options;
    options.handshake_timeout_ms = 30;
    Harness h(options);
    ASSERT_TRUE(h.Wait([&] { return h.closed == 1; }, false));
    EXPECT_EQ(h.last_error, TlsTransport::Error::HandshakeTimeout);
    EXPECT_EQ(h.tls->GetState(), TlsTransport::State::Failed);
    EXPECT_TRUE(h.tcp->IsClosed());
    h.tls->Close();
    EXPECT_EQ(h.closed, 1);
}

TEST(TlsTransportTest, RejectsPlaintextOnTlsConnection)
{
    Harness h;
    const std::string request = "GET / HTTP/1.1\r\n\r\n";
    ASSERT_EQ(::send(h.client->Fd(), request.data(), request.size(), 0), request.size());
    ASSERT_TRUE(h.Wait([&] { return h.closed == 1; }, false));
    EXPECT_EQ(h.last_error, TlsTransport::Error::ProtocolError);
    EXPECT_EQ(h.ready, 0);
}

TEST(TlsTransportTest, InputAndCiphertextLimitsAbortHandshake)
{
    for (bool input : {false, true}) {
        TlsTransport::Options options;
        if (input) options.max_input_bytes = 1;
        else options.max_ciphertext_bytes = 1;
        Harness h(options);
        ASSERT_TRUE(h.Wait([&] { return h.closed == 1; }));
        EXPECT_EQ(h.last_error, TlsTransport::Error::BufferLimit);
        EXPECT_EQ(h.ready, 0);
    }
}

TEST(TlsTransportTest, ClientRejectsUntrustedCertificate)
{
    Harness h({}, 4 * 1024 * 1024, TLS1_2_VERSION, false);
    ASSERT_TRUE(h.Wait([&] { return h.client->failed && h.closed == 1; }));
    EXPECT_EQ(h.last_error, TlsTransport::Error::ProtocolError);
    EXPECT_EQ(h.ready, 0);
}

TEST(TlsTransportTest, PeerCloseNotifyIsAnsweredBeforeTcpCloses)
{
    Harness h({}, 128);
    ASSERT_TRUE(h.Handshake());
    h.client->ShutdownTls();
    ASSERT_TRUE(h.Wait([&] { return h.client->closed && h.closed == 1; }));
    EXPECT_EQ(h.last_error, TlsTransport::Error::None);
    EXPECT_EQ(h.tls->GetState(), TlsTransport::State::Closed);
    EXPECT_TRUE(h.tcp->IsClosed());
}

TEST(TlsTransportTest, AbruptTcpCloseIsReportedSeparately)
{
    Harness h;
    ASSERT_TRUE(h.Handshake());
    h.client->DropTcp();
    ASSERT_TRUE(h.Wait([&] { return h.closed == 1; }, false));
    EXPECT_EQ(h.last_error, TlsTransport::Error::TcpClosed);
    EXPECT_EQ(h.tls->GetState(), TlsTransport::State::Failed);
}

TEST(TlsTransportTest, LocalCancelAndDestructionReleaseConnectionAndTimer)
{
    Harness h;
    h.tls->Close();
    h.tls->Close();
    EXPECT_EQ(h.closed, 1);
    EXPECT_EQ(h.last_error, TlsTransport::Error::LocalClose);
    EXPECT_TRUE(h.tcp->IsClosed());
    EXPECT_EQ(h.tls->Send(reinterpret_cast<const uint8_t*>("x"), 1), TlsTransport::SendResult::Closed);
    Harness second;
    second.tls.reset();
    EXPECT_TRUE(second.tcp->IsClosed());
    EXPECT_EQ(second.closed, 0);
}

} // namespace
