#pragma once
#include "third/nlohmann/json.hpp"
#include "service/Cluster/Coordinator.h"
#include <fstream>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>
#include <map>

namespace server::cluster
{
using Json = nlohmann::json;
inline Json ReadConfig(const std::string& path, std::initializer_list<const char*> keys)
{
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open process config");
    Json config;
    file >> config;
    if (!config.is_object()) throw std::runtime_error("process config must be an object");
    std::set<std::string> allowed(keys.begin(), keys.end());
    for (auto it = config.begin(); it != config.end(); ++it)
        if (!allowed.count(it.key())) throw std::runtime_error("unknown process config field: " + it.key());
    return config;
}
inline std::string Env(const char* name, std::size_t min_size = 1, std::size_t max_size = 4096)
{
    const auto* value = std::getenv(name);
    if (!value || std::string(value).size() < min_size || std::string(value).size() > max_size)
        throw std::runtime_error(std::string("missing/invalid environment variable: ") + name);
    return value;
}
inline uint16_t Port(const Json& config, const char* name)
{
    const auto& value = config.at(name);
    if (!value.is_number_integer() || value < 1 || value > 65535) throw std::runtime_error("invalid port");
    return value.get<uint16_t>();
}
inline int Bounded(const Json& config, const char* name, int low, int high)
{
    const auto& value = config.at(name);
    if (!value.is_number_integer() || value < low || value > high) throw std::runtime_error("invalid capacity");
    return value.get<int>();
}
inline std::string JwtIssuer()
{
    const auto issuer = Env("PACKETIA_JWT_ISSUER", 1, 128);
    if (!service::cluster::Coordinator::ValidId(issuer)) throw std::runtime_error("invalid JWT issuer");
    return issuer;
}
inline std::string JwtSecret() { return Env("PACKETIA_JWT_SECRET", 32); }
inline std::map<std::string, std::string> NodeSecrets()
{
    const auto secrets = Json::parse(Env("PACKETIA_NODE_SECRETS", 2, 65536));
    if (!secrets.is_object() || secrets.empty() || secrets.size() > 256) throw std::runtime_error("invalid node secrets");
    std::map<std::string, std::string> result;
    for (auto it = secrets.begin(); it != secrets.end(); ++it)
    {
        if (!service::cluster::Coordinator::ValidId(it.key()) || !it.value().is_string())
            throw std::runtime_error("invalid node credential");
        const auto secret = it.value().get<std::string>();
        if (secret.size() < 32 || secret.size() > 4096 || secret.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("invalid node credential");
        result.emplace(it.key(), secret);
    }
    return result;
}
}
