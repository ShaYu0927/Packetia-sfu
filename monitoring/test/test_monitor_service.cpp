#include "MonitorService.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
int main()
try
{
    monitoring::MetricsContext context;
    const auto threads_before = utils::ThreadManager::Instance().Snapshot().size();
    service::MonitorService monitor(context.Registry(), std::chrono::milliseconds(100));
    CHECK(!monitor.Start());
    CHECK(monitor.Init() && monitor.Start() && monitor.Start());
    CHECK(monitor.Type() == service::ServiceType::Monitor && monitor.Health().healthy);
    auto first = monitor.Snapshot();
    CHECK(first && !first->resources.process_cpu_percent);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline && !monitor.Snapshot()->resources.process_cpu_percent)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(monitor.Snapshot()->resources.process_cpu_percent.has_value());
    CHECK(monitor.Snapshot()->runtime.captured_ns > first->runtime.captured_ns);
    monitor.Stop(); monitor.Stop();
    CHECK(monitor.State() == service::ServiceState::Stopped && !monitor.Health().healthy);
    CHECK(utils::ThreadManager::Instance().Snapshot().size() == threads_before);
    CHECK(monitor.Start());
    monitor.Shutdown();
    CHECK(monitor.State() == service::ServiceState::Created && !monitor.Snapshot());
    CHECK(monitor.Init() && monitor.Start());
    monitor.Shutdown();
    std::cout << "Monitor lifecycle, periodic resources, immutable publication and thread cleanup passed\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
