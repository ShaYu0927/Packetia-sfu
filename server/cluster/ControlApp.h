#pragma once
#include <memory>
#include <string>

namespace server::cluster
{
class ControlApp final
{
public:
    explicit ControlApp(const std::string& config_path);
    ~ControlApp();
    bool Start();
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
