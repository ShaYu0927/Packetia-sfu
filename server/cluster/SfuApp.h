#pragma once
#include <memory>
#include <string>
namespace server::cluster
{
class SfuApp final
{
public:
    explicit SfuApp(const std::string& config_path);
    ~SfuApp();
    bool Start();
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
