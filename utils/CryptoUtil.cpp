#include "CryptoUtil.h"

#include <limits>
#include <vector>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace utils
{

bool SecureRandomBytes(uint8_t* out, size_t byte_count)
{
    if ((!out && byte_count != 0) || byte_count > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    if (byte_count == 0) return true;
    if (RAND_bytes(out, static_cast<int>(byte_count)) == 1) return true;
    OPENSSL_cleanse(out, byte_count);
    return false;
}

bool SecureRandomHex(size_t byte_count, std::string& out)
{
    out.clear();
    if (byte_count > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        byte_count > out.max_size() / 2) return false;
    if (byte_count == 0) return true;

    std::vector<unsigned char> bytes(byte_count);
    if (!SecureRandomBytes(bytes.data(), bytes.size())) return false;
    constexpr char hex[] = "0123456789abcdef";
    out.reserve(byte_count * 2);
    for (auto byte : bytes)
    {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 15]);
    }
    OPENSSL_cleanse(bytes.data(), bytes.size());
    return true;
}

} // namespace utils
