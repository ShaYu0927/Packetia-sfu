#include "server/ServerLauncher.h"
#include "service/core/IService.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Events = std::vector<std::string>;

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void RequireEvents(const Events& actual, std::initializer_list<const char*> expected)
{
    Events wanted(expected.begin(), expected.end());
    if (actual != wanted) {
        std::string message = "Unexpected lifecycle events; expected:";
        for (const auto& event : wanted) {
            message += " " + event;
        }
        message += "; actual:";
        for (const auto& event : actual) {
            message += " " + event;
        }
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void RequireRejected(Callback&& callback)
{
    try {
        callback();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid service registration was accepted");
}

enum class Outcome { Success, Failure, Exception, UnknownException };

bool Result(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Failure:
        return false;
    case Outcome::Exception:
        throw std::runtime_error("service operation failed");
    case Outcome::UnknownException:
        throw 42;
    case Outcome::Success:
        return true;
    }
    throw std::runtime_error("Invalid test outcome");
}

void AddCustom(server::ServerLauncher& launcher, Events& events,
               std::string name, Outcome start = Outcome::Success,
               Outcome stop = Outcome::Success)
{
    launcher.AddCustomService(name,
        [&events, name, start] {
            events.push_back(name + ".start");
            return Result(start);
        },
        [&events, name, stop] {
            events.push_back(name + ".stop");
            Result(stop);
        });
}

class ManagedService final : public service::IService {
public:
    ManagedService(Events& events, std::string name)
        : events_(events), name_(std::move(name)) {}

    bool Init() override
    {
        events_.push_back(name_ + ".init");
        return Result(init_result);
    }

    bool Start() override
    {
        events_.push_back(name_ + ".start");
        return Result(start_result);
    }

    void Stop() override
    {
        events_.push_back(name_ + ".stop");
        Result(stop_result);
    }

    void Shutdown() override
    {
        events_.push_back(name_ + ".shutdown");
        Result(shutdown_result);
    }

    service::ServiceType Type() const override { return service::ServiceType::Unknown; }
    service::ServiceState State() const override { return service::ServiceState::Created; }
    service::ServiceHealth Health() const override { return {}; }

    Outcome init_result = Outcome::Success;
    Outcome start_result = Outcome::Success;
    Outcome stop_result = Outcome::Success;
    Outcome shutdown_result = Outcome::Success;

private:
    Events& events_;
    std::string name_;
};

void OrderedLifecycleAndRestart()
{
    Events events;
    server::ServerLauncher launcher;
    AddCustom(launcher, events, "workers");
    AddCustom(launcher, events, "recording");
    AddCustom(launcher, events, "listener");

    launcher.StopAll();
    Require(events.empty(), "Stopping a new launcher must not call services");
    Require(launcher.StartAll(), "Initial startup failed");
    Require(launcher.StartAll(), "Repeated startup failed");
    RequireEvents(events, {"workers.start", "recording.start", "listener.start"});

    launcher.StopAll();
    launcher.StopAll();
    RequireEvents(events, {"workers.start", "recording.start", "listener.start",
                           "listener.stop", "recording.stop", "workers.stop"});

    events.clear();
    Require(launcher.StartAll(), "Restart failed");
    launcher.StopAll();
    RequireEvents(events, {"workers.start", "recording.start", "listener.start",
                           "listener.stop", "recording.stop", "workers.stop"});
}

void StartupFailureRollsBackAttemptedService()
{
    for (const auto failure : {Outcome::Failure, Outcome::Exception, Outcome::UnknownException}) {
        Events events;
        server::ServerLauncher launcher;
        AddCustom(launcher, events, "first");
        AddCustom(launcher, events, "second");
        AddCustom(launcher, events, "failing", failure);
        AddCustom(launcher, events, "unattempted");

        Require(!launcher.StartAll(), "Failed startup must report false");
        RequireEvents(events, {"first.start", "second.start", "failing.start",
                               "failing.stop", "second.stop", "first.stop"});
        launcher.StopAll();
        Require(events.size() == 6, "Rollback must clear the active services");
    }
}

void CleanupExceptionsDoNotInterruptRollbackOrStop()
{
    for (const auto exception : {Outcome::Exception, Outcome::UnknownException}) {
        Events events;
        server::ServerLauncher launcher;
        AddCustom(launcher, events, "first");
        AddCustom(launcher, events, "second", Outcome::Success, exception);
        AddCustom(launcher, events, "failing", Outcome::Failure, exception);
        Require(!launcher.StartAll(), "Cleanup failure must not hide startup failure");
        RequireEvents(events, {"first.start", "second.start", "failing.start",
                               "failing.stop", "second.stop", "first.stop"});
        launcher.StopAll();
        Require(events.size() == 6, "Failed cleanup must not leave callbacks active");

        events.clear();
        server::ServerLauncher running;
        AddCustom(running, events, "first");
        AddCustom(running, events, "second", Outcome::Success, exception);
        AddCustom(running, events, "third", Outcome::Success, exception);
        Require(running.StartAll(), "Startup before cleanup exception failed");
        running.StopAll();
        running.StopAll();
        RequireEvents(events, {"first.start", "second.start", "third.start",
                               "third.stop", "second.stop", "first.stop"});
    }
}

void FailedLauncherCanRetry()
{
    Events events;
    server::ServerLauncher launcher;
    AddCustom(launcher, events, "first");
    bool should_fail = true;
    launcher.AddCustomService("retry",
        [&] {
            events.push_back("retry.start");
            return !should_fail;
        },
        [&] { events.push_back("retry.stop"); });
    Require(!launcher.StartAll(), "First startup should fail");
    should_fail = false;
    events.clear();
    Require(launcher.StartAll(), "Retry after rollback failed");
    launcher.StopAll();
    RequireEvents(events, {"first.start", "retry.start", "retry.stop", "first.stop"});
}

void ManagedServicesInitializeAndShutdownOnEveryRun()
{
    Events events;
    server::ServerLauncher launcher;
    AddCustom(launcher, events, "workers");
    auto managed = std::make_shared<ManagedService>(events, "managed");
    launcher.AddService("managed", managed);
    AddCustom(launcher, events, "listener");

    for (int run = 0; run < 2; ++run) {
        events.clear();
        Require(launcher.StartAll(), "Managed startup failed");
        Require(launcher.StartAll(), "Repeated managed startup failed");
        launcher.StopAll();
        launcher.StopAll();
        RequireEvents(events, {"workers.start", "managed.init", "managed.start",
                               "listener.start", "listener.stop", "managed.stop",
                               "managed.shutdown", "workers.stop"});
    }
}

void ManagedPartialFailureReleasesResources()
{
    for (const bool fail_init : {true, false}) {
        for (const auto failure : {Outcome::Failure, Outcome::Exception, Outcome::UnknownException}) {
            Events events;
            server::ServerLauncher launcher;
            AddCustom(launcher, events, "workers");
            auto managed = std::make_shared<ManagedService>(events, "managed");
            (fail_init ? managed->init_result : managed->start_result) = failure;
            launcher.AddService("managed", managed);
            AddCustom(launcher, events, "listener");
            Require(!launcher.StartAll(), "Partial managed failure must report false");
            if (fail_init) {
                RequireEvents(events, {"workers.start", "managed.init", "managed.stop",
                                       "managed.shutdown", "workers.stop"});
            } else {
                RequireEvents(events, {"workers.start", "managed.init", "managed.start",
                                       "managed.stop", "managed.shutdown", "workers.stop"});
            }

            managed->init_result = managed->start_result = Outcome::Success;
            events.clear();
            Require(launcher.StartAll(), "Managed service cannot retry after partial failure");
            launcher.StopAll();
            RequireEvents(events, {"workers.start", "managed.init", "managed.start",
                                   "listener.start", "listener.stop", "managed.stop",
                                   "managed.shutdown", "workers.stop"});
        }
    }
}

void ManagedStopExceptionStillCallsShutdown()
{
    for (const auto exception : {Outcome::Exception, Outcome::UnknownException}) {
        for (const bool fail_start : {false, true}) {
            Events events;
            server::ServerLauncher launcher;
            AddCustom(launcher, events, "workers");
            auto managed = std::make_shared<ManagedService>(events, "managed");
            managed->start_result = fail_start ? Outcome::Failure : Outcome::Success;
            managed->stop_result = exception;
            managed->shutdown_result = exception;
            launcher.AddService("managed", managed);

            Require(launcher.StartAll() == !fail_start, "Unexpected managed startup result");
            launcher.StopAll();
            launcher.StopAll();
            RequireEvents(events, {"workers.start", "managed.init", "managed.start",
                                   "managed.stop", "managed.shutdown", "workers.stop"});
        }
    }
}

struct ListenerState {
    Events events;
    std::string ip;
    uint16_t port = 0;
    int constructor_argument = 0;
};

class Listener {
public:
    Listener(std::shared_ptr<ListenerState> state, std::unique_ptr<int> argument)
        : state_(std::move(state)), argument_(std::move(argument))
    {
        state_->constructor_argument = *argument_;
    }

    ~Listener() { state_->events.push_back("destroy"); }

    bool Start(const std::string& ip, uint16_t port)
    {
        state_->events.push_back("start");
        state_->ip = ip;
        state_->port = port;
        return true;
    }

    void Stop() { state_->events.push_back("stop"); }

private:
    std::shared_ptr<ListenerState> state_;
    std::unique_ptr<int> argument_;
};

void ListenerArgumentsAndOwnedLifetime()
{
    auto state = std::make_shared<ListenerState>();
    std::weak_ptr<Listener> lifetime;
    {
        server::ServerLauncher launcher;
        std::string ip = "127.0.0.1";
        auto listener = launcher.AddIpPortService<Listener>(
            "listener", ip, 18554, state, std::make_unique<int>(73));
        lifetime = listener;
        listener.reset();
        ip = "192.0.2.1";
        Require(!lifetime.expired(), "Launcher must own the registered listener");
        Require(launcher.StartAll(), "Listener startup failed");
        Require(state->ip == "127.0.0.1", "Listener must retain its registered bind address");
        Require(state->port == 18554, "Listener received the wrong port");
        Require(state->constructor_argument == 73, "Move-only constructor argument was lost");
    }
    Require(lifetime.expired(), "Listener remains owned after launcher destruction");
    RequireEvents(state->events, {"start", "stop", "destroy"});
}

void DestructorStopsAllServicesDespiteExceptions()
{
    Events events;
    std::weak_ptr<ManagedService> lifetime;
    {
        server::ServerLauncher launcher;
        AddCustom(launcher, events, "workers", Outcome::Success, Outcome::Exception);
        auto managed = std::make_shared<ManagedService>(events, "managed");
        managed->stop_result = Outcome::UnknownException;
        managed->shutdown_result = Outcome::Exception;
        launcher.AddService("managed", managed);
        lifetime = managed;
        managed.reset();
        Require(!lifetime.expired(), "Launcher must own the managed service");
        Require(launcher.StartAll(), "Startup before destruction failed");
    }
    Require(lifetime.expired(), "Managed service remains owned after launcher destruction");
    RequireEvents(events, {"workers.start", "managed.init", "managed.start",
                           "managed.stop", "managed.shutdown", "workers.stop"});
}

void InvalidRegistrationsDoNotChangeTheServiceSet()
{
    Events events;
    server::ServerLauncher launcher;
    RequireRejected([&] { launcher.AddCustomService("", [] { return true; }, [] {}); });
    RequireRejected([&] { launcher.AddCustomService("missing-start", {}, [] {}); });
    RequireRejected([&] { launcher.AddCustomService("missing-stop", [] { return true; }, {}); });
    RequireRejected([&] { launcher.AddService("null-service", std::shared_ptr<service::IService>{}); });
    AddCustom(launcher, events, "unique");
    RequireRejected([&] { AddCustom(launcher, events, "unique"); });
    RequireRejected([&] {
        launcher.AddService("unique", std::make_shared<ManagedService>(events, "duplicate"));
    });
    RequireRejected([&] {
        launcher.AddIpPortService<Listener>("unique", "127.0.0.1", 1234,
                                           std::make_shared<ListenerState>(), std::make_unique<int>(1));
    });
    Require(launcher.StartAll(), "Rejected registrations damaged valid services");
    launcher.StopAll();
    RequireEvents(events, {"unique.start", "unique.stop"});

    AddCustom(launcher, events, "missing-start");
    AddCustom(launcher, events, "missing-stop");
    AddCustom(launcher, events, "null-service");
    events.clear();
    Require(launcher.StartAll(), "Rejected names should be reusable after correction");
    launcher.StopAll();
    RequireEvents(events, {"unique.start", "missing-start.start", "missing-stop.start",
                           "null-service.start", "null-service.stop", "missing-stop.stop",
                           "missing-start.stop", "unique.stop"});
}

void RegistrationRequiresAStoppedLauncher()
{
    Events events;
    server::ServerLauncher launcher;
    int registration_rejections = 0;
    auto reject_registration = [&] {
        RequireRejected([&] { AddCustom(launcher, events, "late"); });
        ++registration_rejections;
    };
    launcher.AddCustomService("first",
        [&] {
            events.push_back("first.start");
            reject_registration();
            return true;
        },
        [&] {
            events.push_back("first.stop");
            reject_registration();
        });
    AddCustom(launcher, events, "second");

    Require(launcher.StartAll(), "Startup failed when invalid registration was rejected");
    reject_registration();
    launcher.StopAll();
    Require(registration_rejections == 3, "Registration must be rejected during start, run and stop");
    RequireEvents(events, {"first.start", "second.start", "second.stop", "first.stop"});
    AddCustom(launcher, events, "late");
}

void EmptyLauncherHasTheSameLifecycle()
{
    Events events;
    server::ServerLauncher launcher;
    Require(launcher.StartAll(), "An empty launcher should start successfully");
    Require(launcher.StartAll(), "Repeated empty startup should succeed");
    RequireRejected([&] { AddCustom(launcher, events, "late"); });
    launcher.StopAll();
    launcher.StopAll();
    AddCustom(launcher, events, "late");
    Require(launcher.StartAll(), "Registration after stopping an empty launcher failed");
    launcher.StopAll();
    RequireEvents(events, {"late.start", "late.stop"});
}

void ReentrantLifecycleCallsDoNotRepeatCallbacks()
{
    Events events;
    server::ServerLauncher launcher;
    bool nested_start_accepted = false;
    launcher.AddCustomService("first",
        [&] {
            events.push_back("first.start");
            nested_start_accepted |= launcher.StartAll();
            launcher.StopAll();
            return true;
        },
        [&] {
            events.push_back("first.stop");
            nested_start_accepted |= launcher.StartAll();
            launcher.StopAll();
        });
    AddCustom(launcher, events, "second");
    Require(launcher.StartAll(), "Reentrant calls interfered with startup");
    launcher.StopAll();
    Require(!nested_start_accepted, "Startup during a lifecycle callback should be rejected");
    RequireEvents(events, {"first.start", "second.start", "second.stop", "first.stop"});
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"ordered lifecycle and restart", OrderedLifecycleAndRestart},
        {"startup rollback", StartupFailureRollsBackAttemptedService},
        {"cleanup exception isolation", CleanupExceptionsDoNotInterruptRollbackOrStop},
        {"retry after failure", FailedLauncherCanRetry},
        {"managed lifecycle", ManagedServicesInitializeAndShutdownOnEveryRun},
        {"managed partial failure", ManagedPartialFailureReleasesResources},
        {"managed cleanup exceptions", ManagedStopExceptionStillCallsShutdown},
        {"listener arguments and lifetime", ListenerArgumentsAndOwnedLifetime},
        {"destructor cleanup", DestructorStopsAllServicesDespiteExceptions},
        {"invalid registrations", InvalidRegistrationsDoNotChangeTheServiceSet},
        {"registration during lifecycle", RegistrationRequiresAStoppedLauncher},
        {"empty launcher lifecycle", EmptyLauncherHasTheSameLifecycle},
        {"reentrant lifecycle calls", ReentrantLifecycleCallsDoNotRepeatCallbacks},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL " << name << ": unexpected non-standard exception\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
