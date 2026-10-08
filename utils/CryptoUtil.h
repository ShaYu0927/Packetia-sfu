#ifndef PACKETIA_UTILS_CRYPTO_UTIL_H_
#define PACKETIA_UTILS_CRYPTO_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace utils
{

bool SecureRandomBytes(uint8_t* out, size_t byte_count);

// Generates byte_count cryptographically random bytes as lowercase hex.
// Clears out on failure. A zero-byte request succeeds with empty output.
bool SecureRandomHex(size_t byte_count, std::string& out);

} // namespace utils

#endif
