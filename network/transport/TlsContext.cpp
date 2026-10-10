#include "TlsContext.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace network::transport
{

TlsContext::TlsContext(SSL_CTX* context) : context_(context) {}
TlsContext::~TlsContext() { SSL_CTX_free(context_); }

std::shared_ptr<TlsContext> TlsContext::CreateServer(const Options& options, std::string* error)
{
    if (error) error->clear();
    auto fail = [error](const char* reason) -> std::shared_ptr<TlsContext> {
        if (error) {
            *error = reason;
            const auto code = ERR_peek_last_error();
            if (code != 0) {
                char details[256]{};
                ERR_error_string_n(code, details, sizeof(details));
                error->append(": ").append(details);
            }
        }
        ERR_clear_error();
        return {};
    };
    ERR_clear_error();
    if (options.certificate_chain_file.empty() || options.private_key_file.empty())
        return fail("TLS certificate chain and private key are required");

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(
        SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1)
        return fail("Cannot create TLS server context");
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
    // PEM keys must be unencrypted; never prompt for a password at startup.
    SSL_CTX_set_default_passwd_cb(context.get(), [](char*, int, int, void*) { return 0; });
    if (SSL_CTX_use_certificate_chain_file(context.get(), options.certificate_chain_file.c_str()) != 1)
        return fail("Cannot load TLS certificate chain");
    if (SSL_CTX_use_PrivateKey_file(context.get(), options.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1)
        return fail("Cannot load TLS private key");
    if (SSL_CTX_check_private_key(context.get()) != 1)
        return fail("TLS certificate and private key do not match");
    return std::shared_ptr<TlsContext>(new TlsContext(context.release()));
}

} // namespace network::transport
