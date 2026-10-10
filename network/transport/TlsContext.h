#ifndef PACKETIA_NETWORK_TLS_CONTEXT_H
#define PACKETIA_NETWORK_TLS_CONTEXT_H

#include <memory>
#include <string>

struct ssl_ctx_st;

namespace network::transport
{

class TlsTransport;

// Immutable after construction; shared by connections using one certificate.
class TlsContext
{
public:
    struct Options
    {
        std::string certificate_chain_file;
        std::string private_key_file;
    };

    static std::shared_ptr<TlsContext> CreateServer(const Options& options, std::string* error = nullptr);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

private:
    explicit TlsContext(ssl_ctx_st* context);
    friend class TlsTransport;
    ssl_ctx_st* context_ = nullptr;
};

} // namespace network::transport

#endif
