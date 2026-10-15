#include "ScryptPasswordHasher.h"
#include "AccountPolicy.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>

namespace service::account
{
namespace
{
constexpr uint64_t kMaxMemory = 128 * 1024 * 1024;
constexpr std::string_view kParameters = "$scrypt$ln=15,r=8,p=1$";

struct Secret
{
    std::array<unsigned char, 32> bytes{};
    ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

std::string Hex(const unsigned char* bytes, size_t size)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (size_t i = 0; i < size; ++i)
    {
        text[i * 2] = alphabet[bytes[i] >> 4];
        text[i * 2 + 1] = alphabet[bytes[i] & 15];
    }
    return text;
}

bool Unhex(std::string_view text, unsigned char* output, size_t size)
{
    if (text.size() != size * 2) return false;
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < size; ++i)
    {
        const int high = digit(text[i * 2]), low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool Derive(std::string_view password, const unsigned char* salt, size_t salt_size,
            unsigned log_n, Secret& result)
{
    return EVP_PBE_scrypt(password.data(), password.size(), salt, salt_size,
                          uint64_t{1} << log_n, 8, 1, kMaxMemory,
                          result.bytes.data(), result.bytes.size()) == 1;
}
}

bool ScryptPasswordHasher::IsReady() const noexcept
{
    Secret probe;
    const unsigned char salt = 0;
    return EVP_PBE_scrypt("", 0, &salt, 1, 16, 1, 1, 1024 * 1024,
                          probe.bytes.data(), probe.bytes.size()) == 1;
}

AccountResult<std::string> ScryptPasswordHasher::Hash(std::string_view password) const
{
    if (!ValidPasswordInput(password))
        return AccountResult<std::string>::Failure(AccountError::InvalidArgument);
    std::array<unsigned char, 16> salt{};
    Secret derived;
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        !Derive(password, salt.data(), salt.size(), 15, derived))
        return AccountResult<std::string>::Failure(AccountError::InternalError);
    return AccountResult<std::string>::Success(std::string(kParameters) +
        Hex(salt.data(), salt.size()) + "$" + Hex(derived.bytes.data(), derived.bytes.size()));
}

AccountStatus ScryptPasswordHasher::Verify(std::string_view password,
                                          std::string_view encoded_hash) const
{
    if (!ValidPasswordInput(password)) return AccountStatus::Failure(AccountError::InvalidArgument);
    // Strict allowlist bounds CPU/memory before invoking the KDF. Parameters
    // from older supported work factors can be verified without unbounded input.
    constexpr std::string_view prefix = "$scrypt$ln=";
    constexpr std::string_view suffix = ",r=8,p=1$";
    if (encoded_hash.size() != prefix.size() + 2 + suffix.size() + 32 + 1 + 64 ||
        encoded_hash.substr(0, prefix.size()) != prefix ||
        encoded_hash.substr(prefix.size() + 2, suffix.size()) != suffix)
        return AccountStatus::Failure(AccountError::StorageError);
    const auto log_text = encoded_hash.substr(prefix.size(), 2);
    if (log_text != "14" && log_text != "15" && log_text != "16")
        return AccountStatus::Failure(AccountError::StorageError);
    const unsigned log_n = 10 + static_cast<unsigned>(log_text[1] - '0');
    const size_t salt_offset = prefix.size() + 2 + suffix.size();
    std::array<unsigned char, 16> salt{};
    Secret expected, actual;
    if (encoded_hash[salt_offset + 32] != '$' ||
        !Unhex(encoded_hash.substr(salt_offset, 32), salt.data(), salt.size()) ||
        !Unhex(encoded_hash.substr(salt_offset + 33), expected.bytes.data(), expected.bytes.size()))
        return AccountStatus::Failure(AccountError::StorageError);
    if (!Derive(password, salt.data(), salt.size(), log_n, actual))
        return AccountStatus::Failure(AccountError::InternalError);
    return CRYPTO_memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()) == 0
        ? AccountStatus::Success() : AccountStatus::Failure(AccountError::InvalidCredentials);
}

}
