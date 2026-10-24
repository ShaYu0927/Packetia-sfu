#include "SystemResourceCollector.h"
#include <algorithm>
#include <fstream>
#include <string>
#include <thread>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace monitoring
{
ResourceReading SystemResourceCollector::Read()
{
    ResourceReading result;
#if defined(__linux__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        result.process_cpu_seconds = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
            (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
#endif
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    uint64_t virtual_pages = 0, resident_pages = 0;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (statm >> virtual_pages >> resident_pages && page_size > 0)
        result.rss_bytes = resident_pages * static_cast<uint64_t>(page_size);
    std::ifstream memory("/proc/meminfo");
    std::string key, line;
    uint64_t value = 0;
    while (memory >> key >> value)
    {
        if (key == "MemTotal:") result.host_total_bytes = value * 1024;
        if (key == "MemAvailable:") result.host_available_bytes = value * 1024;
        std::getline(memory, line);
    }
    std::ifstream cpu("/proc/stat");
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (cpu >> key >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)
    {
        result.host_total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;
        result.host_idle_ticks = idle + iowait;
    }
#endif
#ifdef __APPLE__
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        result.rss_bytes = info.resident_size;
    uint64_t total = 0;
    size_t size = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0) result.host_total_bytes = total;
    host_cpu_load_info_data_t cpu{};
    count = HOST_CPU_LOAD_INFO_COUNT;
    const auto host = mach_host_self();
    if (host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count) == KERN_SUCCESS)
    {
        for (auto tick : cpu.cpu_ticks) result.host_total_ticks += tick;
        result.host_idle_ticks = cpu.cpu_ticks[CPU_STATE_IDLE];
    }
    vm_statistics64_data_t vm{};
    count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
        result.host_available_bytes = (vm.free_count + vm.inactive_count) * static_cast<uint64_t>(page);
    mach_port_deallocate(mach_task_self(), host);
#endif
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    const auto ticks = [](FILETIME t) { return (uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        result.process_cpu_seconds = (ticks(kernel) + ticks(user)) / 10000000.0;
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) result.rss_bytes = info.WorkingSetSize;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        result.host_total_bytes = memory.ullTotalPhys;
        result.host_available_bytes = memory.ullAvailPhys;
    }
    FILETIME idle{};
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        result.host_total_ticks = ticks(kernel) + ticks(user);
        result.host_idle_ticks = ticks(idle);
    }
#endif
    return result;
}

SystemResources SystemResourceCollector::Collect(uint64_t now)
{
    const auto reading = Read();
    if (!last_ns_ || now - last_ns_ >= 1000000000)
    {
        if (last_ns_)
        {
            if (reading.process_cpu_seconds && previous_.process_cpu_seconds)
                process_percent_ = std::max(0.0, (*reading.process_cpu_seconds - *previous_.process_cpu_seconds) *
                    100000000000.0 / (now - last_ns_));
            if (reading.host_total_ticks > previous_.host_total_ticks && reading.host_idle_ticks >= previous_.host_idle_ticks)
                host_percent_ = std::clamp(100.0 * (1.0 - static_cast<double>(reading.host_idle_ticks - previous_.host_idle_ticks) /
                    (reading.host_total_ticks - previous_.host_total_ticks)), 0.0, 100.0);
        }
        last_ns_ = now;
        previous_ = reading;
    }
    SystemResources result;
    result.logical_cpus = std::thread::hardware_concurrency();
    result.process_cpu_seconds = reading.process_cpu_seconds;
    result.process_rss_bytes = reading.rss_bytes;
    result.host_memory_total_bytes = reading.host_total_bytes;
    result.host_memory_available_bytes = reading.host_available_bytes;
    result.process_cpu_percent = process_percent_;
    result.host_cpu_percent = host_percent_;
    return result;
}
}
