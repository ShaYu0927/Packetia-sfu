#ifndef PACKETIA_SERVICE_ACCOUNT_OPTIONS_H_
#define PACKETIA_SERVICE_ACCOUNT_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace service::account
{

struct AccountOptions
{
    bool enabled = false;
    // Explicit admission policy: enabling the service does not open registration.
    bool registration_enabled = false;
    std::string database_path = "packetia.db";
    uint32_t session_ttl_seconds = 86400;
    uint32_t worker_count = 2;
    size_t max_pending_requests = 128;

    bool IsValid() const
    {
        return !database_path.empty() && session_ttl_seconds > 0 &&
               worker_count >= 1 && worker_count <= 64 &&
               max_pending_requests >= 1 && max_pending_requests <= 65536;
    }
};

}

#endif // PACKETIA_SERVICE_ACCOUNT_OPTIONS_H_
