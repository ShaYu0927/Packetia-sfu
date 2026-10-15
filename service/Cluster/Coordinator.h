#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

struct sqlite3;
namespace service::cluster
{
struct Failure : std::runtime_error
{
    Failure(uint16_t status, const char* code) : std::runtime_error(code), status(status), code(code) {}
    uint16_t status;
    const char* code;
};
struct Node
{
    std::string id, instance, signaling_url;
    int max_rooms = 0;
    int participants = 0;
};
struct Admission
{
    Node node;
    bool can_publish = false;
};

// Single control writer. Transactions also fence a mistakenly started second writer.
// All bindings and retired instances survive a control restart.
class Coordinator final
{
public:
    using Clock = std::function<int64_t()>;
    static constexpr int kLeaseSeconds = 10;
    explicit Coordinator(const std::string& database_path, Clock clock = {});
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    void Register(const Node& node);
    void Heartbeat(const std::string& node, const std::string& instance, int participants);
    std::string CreateRoom(const std::string& owner);
    void AddMember(const std::string& room, const std::string& owner,
                   const std::string& user, bool can_publish);
    Admission Admit(const std::string& room, const std::string& user);
    static int64_t UnixSeconds();
    static std::string RandomId();
    static bool ValidId(const std::string& value);
    static bool ValidSignalingUrl(const std::string& value);
private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    Clock clock_;
};
}
