// Reproducible userspace impairment traces, applied to the production queue.
// UDP mode uses real loopback datagrams and wall-clock owner/tick scheduling.
#include "RtpPacketReorderQueue.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
using Json = nlohmann::json;
constexpr size_t kWireSize = 1200;
constexpr uint16_t kFirstSequence = 65500;
constexpr int64_t kTickNs = 10000000;
struct Options {
    std::string mode = "simulated", scenario = "baseline", samples;
    uint32_t seed = 11, duration_ms = 3000, max_cache = 50, wait_ms = 100;
};
struct Profile {
    int pps = 1000, base_ms = 0, jitter_ms = 0;
    double loss = 0, duplicates = 0, repair = 0;
    int repair_ms = 180;
    bool stall = false;
    int blackout_first = -1, blackout_count = 0;
};
Profile GetProfile(const std::string& name) {
    Profile p;
    if (name == "baseline") p.pps = 50;
    else if (name == "audio_jitter") { p.pps = 50; p.base_ms = 10; p.jitter_ms = 40; p.loss = .01; p.duplicates = .02; }
    else if (name == "video_moderate") { p.base_ms = 10; p.jitter_ms = 80; p.loss = .03; p.duplicates = .02; }
    else if (name == "video_severe") { p.base_ms = 20; p.jitter_ms = 180; p.loss = .10; p.duplicates = .05; }
    else if (name == "delayed_repair") { p.base_ms = 10; p.jitter_ms = 5; p.repair = .05; }
    else if (name == "burst_loss") { p.base_ms = 10; p.jitter_ms = 10; p.blackout_first = 800; p.blackout_count = 200; }
    else if (name == "owner_stall") { p.base_ms = 10; p.jitter_ms = 5; p.stall = true; }
    else if (name == "large_gap") { p.base_ms = 10; p.blackout_first = 800; p.blackout_count = 1200; }
    else if (name == "high_rate") { p.pps = 5000; p.base_ms = 10; p.jitter_ms = 40; p.loss = .01; }
    else throw std::runtime_error("unknown scenario: " + name);
    return p;
}
struct Event { uint32_t id; int64_t generated_ns, due_ns; };
struct Plan {
    std::vector<Event> events;
    uint32_t generated = 0, network_dropped = 0, duplicates = 0, delayed_repairs = 0;
};
Plan MakePlan(const Profile& p, const Options& o) {
    Plan plan;
    plan.generated = uint64_t(p.pps) * o.duration_ms / 1000;
    std::mt19937 random(o.seed);
    auto chance = [&](double probability) { return double(random()) / 4294967296.0 < probability; };
    auto jitter = [&] { return p.jitter_ms ? random() % (p.jitter_ms * 1000000U + 1U) : 0; };
    for (uint32_t id = 0; id < plan.generated; ++id) {
        const int64_t generated = int64_t(id) * 1000000000 / p.pps;
        // Establish sequence origin and leave a clean tail for accounting.
        const bool impair = id >= 16 && id + 16 < plan.generated;
        const bool blackout = p.blackout_first >= 0 && int(id) >= p.blackout_first &&
            int(id) < p.blackout_first + p.blackout_count;
        if (impair && (blackout || (p.stall && id == 970) || chance(p.loss))) {
            ++plan.network_dropped;
            continue;
        }
        int64_t delay = int64_t(p.base_ms) * 1000000 + (impair ? jitter() : 0);
        if (impair && chance(p.repair)) {
            delay += int64_t(p.repair_ms) * 1000000;
            ++plan.delayed_repairs;
        }
        plan.events.push_back({id, generated, generated + delay});
        if (impair && chance(p.duplicates)) {
            ++plan.duplicates;
            plan.events.push_back({id, generated, generated + delay + 3000000});
        }
    }
    std::stable_sort(plan.events.begin(), plan.events.end(), [](const auto& a, const auto& b) {
        return a.due_ns < b.due_ns;
    });
    return plan;
}
struct Packet {
    uint32_t id;
    int64_t generated_ns, sent_ns, received_ns;
    std::array<uint8_t, kWireSize> bytes{};
};
using Queue = RtpPacketReorderQueue<std::shared_ptr<Packet>>;
double Milliseconds(int64_t ns) { return double(ns) / 1e6; }
Json Distribution(std::vector<double> values) {
    if (values.empty()) return {{"count", 0}, {"p50", nullptr}, {"p95", nullptr}, {"p99", nullptr}, {"max", nullptr}};
    std::sort(values.begin(), values.end());
    auto percentile = [&](double p) { return values[size_t(std::ceil(p * values.size())) - 1]; };
    return {{"count", values.size()}, {"p50", percentile(.50)}, {"p95", percentile(.95)},
        {"p99", percentile(.99)}, {"max", values.back()}};
}
struct Measure {
    Queue queue;
    Clock::time_point origin = Clock::now();
    int64_t current_ns = 0;
    size_t received = 0, peak = 0, bad_order = 0, invalid_wire = 0;
    std::optional<uint32_t> last_id;
    std::map<uint32_t, int64_t> pending;
    std::vector<double> queue_ms, total_ms, prequeue_ms, send_lag_ms, input_us, tick_us;
    std::optional<int64_t> resume_arrived_ns, resume_emitted_ns;
    int resume_id = -1;
    std::ofstream samples;
    Measure(const Options& o, const Profile& p) : queue(1000, o.max_cache, o.wait_ms) {
        if (p.blackout_first >= 0) resume_id = p.blackout_first + p.blackout_count;
        if (!o.samples.empty()) {
            samples.open(o.samples);
            if (!samples) throw std::runtime_error("cannot open samples file");
            samples << "elapsed_ms,buffered,oldest_wait_ms,received,emitted,lost,late,out_of_window\n";
        }
        queue.setOnPacketSorted([this](uint16_t seq, std::shared_ptr<Packet> packet) {
            if (seq != uint16_t(kFirstSequence + packet->id) || (last_id && packet->id <= *last_id)) ++bad_order;
            last_id = packet->id;
            pending.erase(packet->id);
            queue_ms.push_back(Milliseconds(current_ns - packet->received_ns));
            total_ms.push_back(Milliseconds(current_ns - packet->generated_ns));
            if (resume_id >= 0 && int(packet->id) >= resume_id && !resume_emitted_ns)
                resume_emitted_ns = current_ns;
        });
    }
    int64_t Now() const { return std::chrono::duration_cast<Ns>(Clock::now() - origin).count(); }
    void Input(std::shared_ptr<Packet> packet, int64_t now) {
        current_ns = now;
        packet->received_ns = now;
        ++received;
        prequeue_ms.push_back(Milliseconds(now - packet->sent_ns));
        if (resume_id >= 0 && int(packet->id) >= resume_id && !resume_arrived_ns) resume_arrived_ns = now;
        const auto begin = Clock::now();
        queue.inputPacket(uint16_t(kFirstSequence + packet->id), packet, origin + Ns(now));
        input_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        if (packet.use_count() > 1) pending[packet->id] = now;
        peak = std::max(peak, queue.getBufferedCount());
    }
    void Tick(int64_t now) {
        current_ns = now;
        if (samples) {
            int64_t oldest = now;
            for (const auto& entry : pending) oldest = std::min(oldest, entry.second);
            const auto& s = queue.getReorderStats();
            samples << Milliseconds(now) << ',' << queue.getBufferedCount() << ',' << Milliseconds(now - oldest)
                << ',' << received << ',' << s.emitted << ',' << s.lost << ',' << s.late << ',' << s.out_of_window << '\n';
        }
        const auto begin = Clock::now();
        queue.flushExpired(origin + Ns(now));
        tick_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
    }
    Json Result(const Options& o, const Profile& p, const Plan& plan) {
        const auto& s = queue.getReorderStats();
        Json result = {{"mode", o.mode}, {"scenario", o.scenario}, {"seed", o.seed}, {"duration_ms", o.duration_ms},
            {"pps", p.pps}, {"wire_bytes", kWireSize}, {"max_cache", o.max_cache}, {"wait_ms", o.wait_ms},
            {"tick_ms", 10}, {"generated", plan.generated}, {"network_dropped", plan.network_dropped},
            {"duplicates_injected", plan.duplicates}, {"delayed_repairs", plan.delayed_repairs},
            {"scheduled_datagrams", plan.events.size()}, {"received", received}, {"emitted", s.emitted},
            {"lost_sequences", s.lost}, {"duplicates", s.duplicates}, {"late", s.late}, {"out_of_window", s.out_of_window},
            {"timeouts", s.timeouts}, {"overflows", s.overflows}, {"peak_buffered", peak},
            {"resync_attempts", s.resync_attempts}, {"resyncs", s.resyncs},
            {"final_buffered", queue.getBufferedCount()}, {"output_order_errors", bad_order}, {"invalid_wire", invalid_wire},
            {"queue_wait_ms", Distribution(queue_ms)}, {"source_to_output_ms", Distribution(total_ms)},
            {"udp_send_to_queue_ms", Distribution(prequeue_ms)}, {"sender_schedule_lag_ms", Distribution(send_lag_ms)},
            {"input_call_us", Distribution(input_us)}, {"tick_call_us", Distribution(tick_us)}};
        result["resume_arrival_to_output_ms"] = resume_arrived_ns && resume_emitted_ns
            ? Json(Milliseconds(*resume_emitted_ns - *resume_arrived_ns)) : Json(nullptr);
        result["tail_emitted"] = last_id && *last_id == plan.generated - 1;
        result["invariants_ok"] = bad_order == 0 && peak <= o.max_cache && queue.getBufferedCount() == 0 &&
            received == s.emitted + s.duplicates + s.late + s.out_of_window && pending.empty();
        result["recovery_ok"] = o.scenario != "large_gap" ||
            (result["tail_emitted"].get<bool>() && s.resyncs == 1);
        return result;
    }
};
Json Simulated(const Options& o, const Profile& p, const Plan& plan) {
    Measure m(o, p);
    size_t next = 0;
    int64_t tick = 0;
    const int64_t end = int64_t(o.duration_ms + o.wait_ms + 1000) * 1000000;
    bool stalled = false;
    while (next < plan.events.size() || tick <= end) {
        const int64_t delivery = next < plan.events.size() ? plan.events[next].due_ns : INT64_MAX;
        int64_t now = std::min(delivery, tick);
        if (p.stall && !stalled && now >= 1000000000) { stalled = true; now = 1250000000; }
        now = std::max(now, m.current_ns);
        while (next < plan.events.size() && plan.events[next].due_ns <= now) {
            const auto e = plan.events[next++];
            auto packet = std::make_shared<Packet>();
            packet->id = e.id;
            packet->generated_ns = e.generated_ns;
            packet->sent_ns = e.due_ns;
            m.Input(std::move(packet), now);
        }
        if (tick <= now) { m.Tick(now); tick = now + kTickNs; }
        if (next == plan.events.size() && tick > end) break;
    }
    return m.Result(o, p, plan);
}
class Socket {
public:
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    Socket() { if (fd < 0) throw std::runtime_error("socket failed"); }
    ~Socket() { ::close(fd); }
    Socket(const Socket&) = delete;
};
void Store32(uint8_t* out, uint32_t value) { for (int i = 3; i >= 0; --i) { out[i] = uint8_t(value); value >>= 8; } }
uint32_t Load32(const uint8_t* in) { return uint32_t(in[0]) << 24 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 8 | in[3]; }
void Store64(uint8_t* out, uint64_t value) { Store32(out, value >> 32); Store32(out + 4, value); }
int64_t Load64(const uint8_t* in) { return int64_t(uint64_t(Load32(in)) << 32 | Load32(in + 4)); }
Json Udp(const Options& o, const Profile& p, const Plan& plan) {
    Measure m(o, p);
    Socket receiver, sender;
    int requested_buffer = 1024 * 1024;
    const bool buffer_set = setsockopt(receiver.fd, SOL_SOCKET, SO_RCVBUF, &requested_buffer, sizeof(requested_buffer)) == 0;
    int actual_buffer = 0;
    socklen_t option_size = sizeof(actual_buffer);
    getsockopt(receiver.fd, SOL_SOCKET, SO_RCVBUF, &actual_buffer, &option_size);
    if (fcntl(receiver.fd, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("nonblocking socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(receiver.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("bind failed");
    socklen_t length = sizeof(address);
    if (getsockname(receiver.fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) throw std::runtime_error("getsockname failed");
    std::atomic<size_t> send_errors{0}, sent{0};
    std::vector<double> schedule_lag;
    std::thread network([&] {
        std::array<uint8_t, kWireSize> wire{};
        wire.fill(0x5A);
        wire[0] = 0x80; wire[1] = 96;
        Store32(wire.data() + 8, 42);
        for (const auto& e : plan.events) {
            std::this_thread::sleep_until(m.origin + Ns(e.due_ns));
            const uint16_t seq = uint16_t(kFirstSequence + e.id);
            wire[2] = uint8_t(seq >> 8); wire[3] = uint8_t(seq);
            Store32(wire.data() + 4, uint64_t(e.id) * 90000 / p.pps);
            Store32(wire.data() + 12, e.id);
            Store64(wire.data() + 16, e.generated_ns);
            const int64_t sent_ns = m.Now();
            Store64(wire.data() + 24, sent_ns);
            schedule_lag.push_back(Milliseconds(sent_ns - e.due_ns));
            if (sendto(sender.fd, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != ssize_t(wire.size())) ++send_errors;
            else ++sent;
        }
    });
    int64_t tick = 0;
    bool stalled = false;
    int receive_error = 0;
    const int64_t end = std::max(int64_t(o.duration_ms) * 1000000, plan.events.back().due_ns) +
        int64_t(o.wait_ms + 500) * 1000000;
    while (m.Now() < end && !receive_error) {
        auto now = m.Now();
        if (p.stall && !stalled && now >= 1000000000) {
            stalled = true;
            std::this_thread::sleep_until(m.origin + Ns(1250000000));
            now = m.Now();
        }
        if (now >= tick) { m.Tick(now); tick = now + kTickNs; }
        pollfd descriptor{receiver.fd, POLLIN, 0};
        const auto remaining = std::max<int64_t>(0, tick - m.Now());
        const int status = poll(&descriptor, 1, int((remaining + 999999) / 1000000));
        if (status < 0) { if (errno == EINTR) continue; receive_error = errno; break; }
        // Limit each batch so continuous traffic cannot starve the tick.
        for (int batch = 0; status > 0 && batch < 64; ++batch) {
            auto packet = std::make_shared<Packet>();
            const auto bytes = recv(receiver.fd, packet->bytes.data(), packet->bytes.size(), 0);
            if (bytes < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) receive_error = errno;
                break;
            }
            if (bytes != kWireSize || packet->bytes[0] != 0x80 || packet->bytes[1] != 96 || Load32(packet->bytes.data() + 8) != 42) {
                ++m.invalid_wire; continue;
            }
            packet->id = Load32(packet->bytes.data() + 12);
            const uint16_t wire_seq = uint16_t(packet->bytes[2]) << 8 | packet->bytes[3];
            if (wire_seq != uint16_t(kFirstSequence + packet->id)) { ++m.invalid_wire; continue; }
            packet->generated_ns = Load64(packet->bytes.data() + 16);
            packet->sent_ns = Load64(packet->bytes.data() + 24);
            m.Input(std::move(packet), m.Now());
        }
    }
    network.join();
    m.Tick(m.Now());
    m.send_lag_ms = std::move(schedule_lag);
    auto result = m.Result(o, p, plan);
    result["udp_sent"] = sent.load();
    result["send_errors"] = send_errors.load();
    result["receive_error"] = receive_error;
    result["local_delivery_gap"] = int64_t(sent) - int64_t(m.received);
    result["socket_receive_buffer_bytes"] = actual_buffer;
    result["socket_buffer_request_succeeded"] = buffer_set;
    result["invariants_ok"] = result["invariants_ok"].get<bool>() && send_errors == 0 && receive_error == 0 && m.invalid_wire == 0;
    return result;
}
}

int main(int argc, char** argv) {
    try {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing option value");
            const std::string value = argv[++i];
            if (key == "--mode") o.mode = value;
            else if (key == "--scenario") o.scenario = value;
            else if (key == "--seed") o.seed = std::stoul(value);
            else if (key == "--duration-ms") o.duration_ms = std::stoul(value);
            else if (key == "--max-cache") o.max_cache = std::stoul(value);
            else if (key == "--wait-ms") o.wait_ms = std::stoul(value);
            else if (key == "--samples") o.samples = value;
            else throw std::runtime_error("unknown option: " + key);
        }
        if (o.duration_ms < 2500 || o.duration_ms > 600000 || o.max_cache >= 32768 || o.wait_ms > 10000)
            throw std::runtime_error("invalid benchmark bounds");
        const auto profile = GetProfile(o.scenario);
        const auto plan = MakePlan(profile, o);
        if (o.mode != "simulated" && o.mode != "udp") throw std::runtime_error("mode must be simulated or udp");
        auto result = o.mode == "udp" ? Udp(o, profile, plan) : Simulated(o, profile, plan);
        std::cout << result.dump() << '\n';
        return result["invariants_ok"].get<bool>() && result["recovery_ok"].get<bool>() ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
