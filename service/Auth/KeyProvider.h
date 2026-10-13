#ifndef PACKETIA_SERVICE_AUTH_KEY_PROVIDER_H_
#define PACKETIA_SERVICE_AUTH_KEY_PROVIDER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace service::auth
{

// Implementations are shared by connections and must support concurrent reads.
// Issuer lookup selects a verification key; it does not authenticate the issuer.
class IKeyProvider
{
public:
    virtual ~IKeyProvider() = default;
    virtual std::optional<std::string> SecretFor(std::string_view issuer) const = 0;
};

class StaticKeyProvider final : public IKeyProvider
{
public:
    explicit StaticKeyProvider(std::map<std::string, std::string> keys)
        : keys_(std::move(keys)) {}

    std::optional<std::string> SecretFor(std::string_view issuer) const override
    {
        const auto found = keys_.find(std::string(issuer));
        if (found == keys_.end()) return std::nullopt;
        return found->second;
    }

private:
    const std::map<std::string, std::string> keys_;
};

}

#endif // PACKETIA_SERVICE_AUTH_KEY_PROVIDER_H_
