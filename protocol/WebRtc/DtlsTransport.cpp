#include "DtlsTransport.h"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <utility>

namespace protocol::webrtc
{
namespace
{
constexpr size_t kMaxDatagram = 65507;
constexpr size_t kMaxQueuedDatagrams = 64;
constexpr long kDtlsMtu = 1200;
constexpr uint64_t kHandshakeTimeoutMs = 15000;
constexpr size_t kFingerprintSize = 32;
constexpr size_t kSrtpKeySize = 16;
constexpr size_t kSrtpSaltSize = 14;
using Fingerprint = std::array<unsigned char, kFingerprintSize>;

uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}


struct DatagramIo
{
    std::deque<std::vector<uint8_t>> incoming;
    std::deque<std::vector<uint8_t>> outgoing;
    long mtu = kDtlsMtu;
    bool peek = false;
};

int BioCreate(BIO* bio)
{
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}

int BioDestroy(BIO* bio)
{
    if (!bio) return 0;
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}

int BioRead(BIO* bio, char* data, int size)
{
    BIO_clear_retry_flags(bio);
    auto* io = static_cast<DatagramIo*>(BIO_get_data(bio));
    if (!io || !data || size <= 0) return 0;
    if (io->incoming.empty())
    {
        BIO_set_retry_read(bio);
        return -1;
    }
    const auto& packet = io->incoming.front();
    const auto copied = std::min(packet.size(), static_cast<size_t>(size));
    std::memcpy(data, packet.data(), copied);
    if (!io->peek) io->incoming.pop_front();
    return static_cast<int>(copied);
}

int BioWrite(BIO* bio, const char* data, int size)
{
    BIO_clear_retry_flags(bio);
    auto* io = static_cast<DatagramIo*>(BIO_get_data(bio));
    if (!io || !data || size <= 0 || static_cast<size_t>(size) > kMaxDatagram ||
        io->outgoing.size() >= kMaxQueuedDatagrams) return -1;
    try
    {
        io->outgoing.emplace_back(data, data + size);
        return size;
    }
    catch (...)
    {
        return -1;
    }
}

long BioControl(BIO* bio, int command, long value, void*)
{
    auto* io = static_cast<DatagramIo*>(BIO_get_data(bio));
    if (!io) return 0;
    switch (command)
    {
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
    case BIO_CTRL_DGRAM_SET_DONT_FRAG:
        return 1;
    case BIO_CTRL_PENDING:
        return io->incoming.empty() ? 0 : static_cast<long>(io->incoming.front().size());
    case BIO_CTRL_WPENDING:
        return io->outgoing.empty() ? 0 : static_cast<long>(io->outgoing.front().size());
    case BIO_CTRL_EOF:
        return io->incoming.empty() ? 1 : 0;
    case BIO_CTRL_DGRAM_QUERY_MTU:
    case BIO_CTRL_DGRAM_GET_MTU:
    case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
        return io->mtu;
    case BIO_CTRL_DGRAM_SET_MTU:
        if (value >= 576 && value <= static_cast<long>(kMaxDatagram)) io->mtu = value;
        return io->mtu;
    case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
        return 28;
    case BIO_CTRL_DGRAM_SET_PEEK_MODE:
        io->peek = value != 0;
        return 1;
    default:
        return 0;
    }
}

const BIO_METHOD* DatagramBioMethod()
{
    static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method([]
    {
        BIO_METHOD* result = BIO_meth_new(BIO_TYPE_DGRAM, "Packetia DTLS datagrams");
        if (result && (!BIO_meth_set_create(result, BioCreate) ||
                       !BIO_meth_set_destroy(result, BioDestroy) ||
                       !BIO_meth_set_read(result, BioRead) ||
                       !BIO_meth_set_write(result, BioWrite) ||
                       !BIO_meth_set_ctrl(result, BioControl)))
        {
            BIO_meth_free(result);
            result = nullptr;
        }
        return result;
    }(), BIO_meth_free);
    return method.get();
}

int HexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool DecodeFingerprint(const std::string& value, Fingerprint& bytes)
{
    if (value.size() != bytes.size() * 3 - 1) return false;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        const int high = HexDigit(value[i * 3]);
        const int low = HexDigit(value[i * 3 + 1]);
        if (high < 0 || low < 0 || (i + 1 < bytes.size() && value[i * 3 + 2] != ':')) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

std::string EncodeFingerprint(const Fingerprint& bytes)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 3 - 1);
    for (const auto byte : bytes)
    {
        if (!result.empty()) result.push_back(':');
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

class OpenSslDtlsTransport final : public DtlsTransport
{
public:
    ~OpenSslDtlsTransport() override { Close(); }

    /* 创建 OpenSSL 上下文，生成 EC 密钥和自签名证书，计算本地 SHA-256 指纹 */
    bool Initialize()
    {
        ERR_clear_error();
        context_.reset(SSL_CTX_new(DTLS_method()));
        if (!context_ || SSL_CTX_set_min_proto_version(context_.get(), DTLS1_2_VERSION) != 1 ||
            SSL_CTX_set_tlsext_use_srtp(context_.get(), "SRTP_AES128_CM_SHA1_80") != 0) return false;
        SSL_CTX_set_options(context_.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET |
            SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_QUERY_MTU);
        SSL_CTX_set_session_cache_mode(context_.get(), SSL_SESS_CACHE_OFF);
        // WebRTC authenticates the certificate with the SDP fingerprint.
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
            [](int, X509_STORE_CTX*) { return 1; });
        if (SSL_CTX_set_cipher_list(context_.get(),
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384") != 1) return false;

        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* generated = nullptr;
        if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) != 1 ||
            EVP_PKEY_keygen(generator.get(), &generated) != 1) return false;
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 365L * 24 * 60 * 60) ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) return false;
        X509_NAME* name = X509_get_subject_name(certificate.get());
        const unsigned char commonName[] = "Packetia WebRTC";
        if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, commonName, -1, -1, 0) != 1 ||
            X509_set_issuer_name(certificate.get(), name) != 1 ||
            X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 ||
            SSL_CTX_use_certificate(context_.get(), certificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(context_.get(), key.get()) != 1 ||
            SSL_CTX_check_private_key(context_.get()) != 1) return false;
        Fingerprint digest{};
        unsigned int size = 0;
        if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &size) != 1 ||
            size != digest.size()) return false;
        local_.setup = DtlsSetup::ActPass;
        local_.fingerprints.push_back({"sha-256", EncodeFingerprint(digest)});
        return true;
    }
    /* 提供本地指纹与 setup 参数，用于生成 SDP */
    DtlsParameters LocalParameters() const override { return local_; }

    /* 保存远端 SDP 指纹，确定本端主动或被动握手角色 */
    bool Configure(const DtlsParameters& remote, DtlsSetup localSetup) override
    {
        if (!context_ || ssl_ || configured_ || closed_ ||
            (localSetup != DtlsSetup::Active && localSetup != DtlsSetup::Passive)) return false;
        if ((remote.setup == DtlsSetup::Active && localSetup != DtlsSetup::Passive) ||
            (remote.setup == DtlsSetup::Passive && localSetup != DtlsSetup::Active) ||
            remote.setup == DtlsSetup::HoldConn) return false;
        bool found = false;
        for (const auto& fingerprint : remote.fingerprints)
        {
            std::string algorithm = fingerprint.algorithm;
            for (auto& c : algorithm) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (algorithm != "sha-256") continue;
            Fingerprint decoded{};
            if (!DecodeFingerprint(fingerprint.value, decoded) || (found && decoded != remoteFingerprint_)) return false;
            remoteFingerprint_ = decoded;
            found = true;
        }
        if (!found) return false;
        local_.setup = localSetup;
        configured_ = true;
        return true;
    }

    /* 创建 SSL 和 BIO，启动握手 */
    bool Start(SendCallback send) override
    {
        if (ssl_ || !configured_ || closed_ || !send) return false;
        const auto* method = DatagramBioMethod();
        if (!method) return Fail();
        ssl_.reset(SSL_new(context_.get()));
        BIO* bio = BIO_new(method);
        if (!ssl_ || !bio)
        {
            BIO_free(bio);
            return Fail();
        }
        BIO_set_data(bio, &io_);
        SSL_set_bio(ssl_.get(), bio, bio);
        if (SSL_set_mtu(ssl_.get(), kDtlsMtu) <= 0) return Fail();
        if (local_.setup == DtlsSetup::Active) SSL_set_connect_state(ssl_.get());
        else SSL_set_accept_state(ssl_.get());
        send_ = std::move(send);
        startedMs_ = NowMs();
        return Drive();
    }

    /* 处理传入的 DTLS 数据报 */
    bool HandleDatagram(const uint8_t* data, size_t size) override
    {
        if (!ssl_ || closed_) return false;
        if (!data || size == 0 || size > kMaxDatagram || io_.incoming.size() >= kMaxQueuedDatagrams) return true;
        try
        {
            io_.incoming.emplace_back(data, data + size);
            return Drive();
        }
        catch (...)
        {
            return Fail();
        }
    }

    bool Tick(uint64_t nowMs) override
    {
        if (!ssl_ || closed_) return false;
        if (!connected_ && nowMs >= startedMs_ && nowMs - startedMs_ >= kHandshakeTimeoutMs) return Fail();
        ERR_clear_error();
        if (DTLSv1_handle_timeout(ssl_.get()) < 0) return Fail();
        return Flush();
    }

    bool IsConnected() const noexcept override { return connected_ && !closed_; }

    /* 握手成功后导出发送、接收方向的 SRTP 密钥 */
    bool ExportSrtpKeys(SrtpKeyingMaterial& keys) const override
    {
        if (!IsConnected()) return false;
        const auto* profile = SSL_get_selected_srtp_profile(ssl_.get());
        if (!profile || profile->id != SRTP_AES128_CM_SHA1_80) return false;
        constexpr size_t keyBytes = kSrtpKeySize + kSrtpSaltSize;
        std::array<unsigned char, keyBytes * 2> material{};
        constexpr char label[] = "EXTRACTOR-dtls_srtp";
        if (SSL_export_keying_material(ssl_.get(), material.data(), material.size(), label, sizeof(label) - 1, nullptr, 0, 0) != 1)
        {
            OPENSSL_cleanse(material.data(), material.size());
            return false;
        }
        try
        {
            // RFC 5764: client key, server key, client salt, server salt.
            const size_t sendSide = local_.setup == DtlsSetup::Active ? 0 : 1;
            SrtpKeyingMaterial result;
            result.profile = static_cast<uint16_t>(profile->id);
            auto extract = [&](size_t side, std::vector<uint8_t>& key)
            {
                key.assign(material.begin() + side * kSrtpKeySize,
                    material.begin() + (side + 1) * kSrtpKeySize);
                key.insert(key.end(), material.begin() + 2 * kSrtpKeySize + side * kSrtpSaltSize,
                    material.begin() + 2 * kSrtpKeySize + (side + 1) * kSrtpSaltSize);
            };
            extract(sendSide, result.sendKey);
            extract(1 - sendSide, result.receiveKey);
            keys.profile = result.profile;
            keys.sendKey.swap(result.sendKey);
            keys.receiveKey.swap(result.receiveKey);
        }
        catch (...)
        {
            OPENSSL_cleanse(material.data(), material.size());
            return false;
        }
        OPENSSL_cleanse(material.data(), material.size());
        return true;
    }

    void Close() noexcept override
    {
        closed_ = true;
        connected_ = false;
        configured_ = false;
        send_ = {};
        ssl_.reset();
        context_.reset();
        io_.incoming.clear();
        io_.outgoing.clear();
        local_.fingerprints.clear();
        OPENSSL_cleanse(remoteFingerprint_.data(), remoteFingerprint_.size());
    }

private:
    bool Fail()
    {
        Close();
        return false;
    }

    bool VerifyPeer()
    {
        std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get_peer_certificate(ssl_.get()), X509_free);
        Fingerprint fingerprint{};
        unsigned int size = 0;
        const auto* profile = SSL_get_selected_srtp_profile(ssl_.get());
        return peer && X509_digest(peer.get(), EVP_sha256(), fingerprint.data(), &size) == 1 &&
            size == fingerprint.size() &&
            CRYPTO_memcmp(fingerprint.data(), remoteFingerprint_.data(), fingerprint.size()) == 0 &&
            profile && profile->id == SRTP_AES128_CM_SHA1_80;
    }

    bool Drive()
    {
        ERR_clear_error();
        if (!connected_)
        {
            const int result = SSL_do_handshake(ssl_.get());
            const int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_.get(), result);
            if (result == 1)
            {
                if (!VerifyPeer()) return Fail();
                connected_ = true;
            }
            else if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) return Fail();
        }
        else
        {
            // Process retransmitted handshake records and close alerts. The
            // application uses SRTP; DTLS application data is discarded.
            std::array<unsigned char, 16384> ignored{};
            for (size_t i = 0; i < kMaxQueuedDatagrams; ++i)
            {
                const int result = SSL_read(ssl_.get(), ignored.data(), static_cast<int>(ignored.size()));
                if (result > 0) continue;
                const int error = SSL_get_error(ssl_.get(), result);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) break;
                return Fail();
            }
        }
        return Flush();
    }

    bool Flush()
    {
        try
        {
            while (!io_.outgoing.empty())
            {
                auto packet = std::move(io_.outgoing.front());
                io_.outgoing.pop_front();
                // A failed UDP send may be transient. OpenSSL retains the
                // handshake flight and Tick will retransmit it.
                if (send_) send_(packet.data(), packet.size());
            }
            return !closed_;
        }
        catch (...)
        {
            return Fail();
        }
    }

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{nullptr, SSL_free};
    DatagramIo io_;
    DtlsParameters local_;
    Fingerprint remoteFingerprint_{};
    SendCallback send_;
    uint64_t startedMs_ = 0;
    bool configured_ = false;
    bool connected_ = false;
    bool closed_ = false;
};
} // namespace

std::unique_ptr<DtlsTransport> CreateDtlsTransport()
{
    try
    {
        auto transport = std::make_unique<OpenSslDtlsTransport>();
        return transport->Initialize() ? std::move(transport) : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
}

} // namespace protocol::webrtc
