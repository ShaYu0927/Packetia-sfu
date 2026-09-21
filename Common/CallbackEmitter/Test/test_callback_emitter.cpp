#include "CallbackEmitter.h"
#include "SourceBase.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_failure {
// Fail exactly one allocation on the calling thread. Other threads and the
// reporting machinery remain unaffected by a deliberately failed operation.
thread_local int remaining = -1;

void* allocate(std::size_t size)
{
    if (remaining == 0) {
        remaining = -1;
        throw std::bad_alloc();
    }
    if (remaining > 0) --remaining;
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}

class FailAfter {
public:
    explicit FailAfter(int successfulAllocations) { remaining = successfulAllocations; }
    ~FailAfter() { remaining = -1; }
    FailAfter(const FailAfter&) = delete;
    FailAfter& operator=(const FailAfter&) = delete;
};
} // namespace allocation_failure

void* operator new(std::size_t size) { return allocation_failure::allocate(size); }
void* operator new[](std::size_t size) { return allocation_failure::allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

static_assert(!std::is_copy_constructible<SignalCOW<int>>::value,
              "Signal identity must not be copied");
static_assert(!std::is_copy_assignable<SignalCOW<int>>::value,
              "Signal identity must not be copy-assigned");
static_assert(!std::is_move_constructible<SignalCOW<int>>::value,
              "Signal identity must not be moved");
static_assert(!std::is_move_assignable<SignalCOW<int>>::value,
              "Signal identity must not be move-assigned");

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

class Gate {
public:
    void open()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ = false;
};

void raiiAndEmptySubscriptions()
{
    SignalCOW<int> signal;
    CHECK(signal.size() == 0);
    CHECK(!signal.subscribe({}));
    int sum = 0;
    {
        auto token = signal.subscribe([&](int value) { sum += value; });
        CHECK(signal.size() == 1);
        signal.emit(3);
        CHECK(sum == 3);
    }
    CHECK(signal.size() == 0);
    signal.emit(7);
    CHECK(sum == 3);

    auto token = signal.subscribe([&](int value) { sum += value; });
    token->cancel();
    token->cancel();
    CHECK(signal.size() == 0);
    signal.emit(7);
    CHECK(sum == 3);
}

void tokenOutlivesOwner()
{
    std::shared_ptr<ISubscription> signalToken;
    {
        SignalCOW<int> signal;
        signalToken = signal.subscribe([](int) {});
    }
    signalToken->cancel();
    signalToken->cancel();
    signalToken.reset();

    std::shared_ptr<ISubscription> sourceToken;
    int received = 0;
    {
        SourceCOW<int> source;
        sourceToken = source.subscribe([&](const int& value) { received = value; });
        source.publish(42);
        CHECK(received == 42);
        CHECK(source.subscriberCount() == 1);
    }
    // Destruction, without an explicit cancel, must also tolerate a dead owner.
    sourceToken.reset();
}

void concurrentRepeatedCancellation()
{
    SignalCOW<> signal;
    std::atomic<int> calls{0};
    auto token = signal.subscribe([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    Gate start;
    std::vector<std::thread> cancellers;
    for (int i = 0; i < 8; ++i) {
        // Each thread has its own stable shared_ptr; the test does not race
        // shared_ptr assignment or token destruction against a member call.
        cancellers.emplace_back([token, &start] {
            start.wait();
            for (int repetition = 0; repetition < 100; ++repetition) token->cancel();
        });
    }
    std::thread emitter([&] {
        start.wait();
        for (int repetition = 0; repetition < 1000; ++repetition) signal.emit();
    });
    start.open();
    for (auto& canceller : cancellers) canceller.join();
    emitter.join();
    CHECK(signal.size() == 0);
    const int callsAfterCancellation = calls.load(std::memory_order_relaxed);
    signal.emit();
    CHECK(calls.load(std::memory_order_relaxed) == callsAfterCancellation);
}

void cancellationDoesNotWaitForRunningCallback()
{
    SignalCOW<> signal;
    Gate entered;
    Gate release;
    std::atomic<int> calls{0};
    auto token = signal.subscribe([&] {
        calls.fetch_add(1, std::memory_order_relaxed);
        entered.open();
        release.wait();
    });
    std::thread emitter([&] { signal.emit(); });
    entered.wait();
    // The callback cannot finish until cancel returns and this thread opens
    // release. A blocking cancel would fail the suite's CTest timeout.
    token->cancel();
    const auto remaining = signal.size();
    signal.emit();
    release.open();
    emitter.join();
    CHECK(remaining == 0);
    CHECK(calls.load(std::memory_order_relaxed) == 1);
}

void canceledLaterEntryIsSkipped()
{
    SignalCOW<> signal;
    Gate entered;
    Gate release;
    int laterCalls = 0;
    auto first = signal.subscribe([&] {
        entered.open();
        release.wait();
    });
    auto later = signal.subscribe([&] { ++laterCalls; });
    std::thread emitter([&] { signal.emit(); });
    entered.wait();
    later->cancel();
    release.open();
    emitter.join();
    CHECK(laterCalls == 0);
    CHECK(signal.size() == 1);
}

void cancelOtherAndSelfFromCallback()
{
    SignalCOW<> signal;
    int firstCalls = 0;
    int laterCalls = 0;
    std::shared_ptr<ISubscription> first;
    std::shared_ptr<ISubscription> later;
    first = signal.subscribe([&] {
        ++firstCalls;
        first->cancel();
        later->cancel();
    });
    later = signal.subscribe([&] { ++laterCalls; });
    signal.emit();
    signal.emit();
    CHECK(firstCalls == 1);
    CHECK(laterCalls == 0);
    CHECK(signal.size() == 0);
}

void subscribeAndEmitFromCallback()
{
    SignalCOW<int> signal;
    std::vector<int> observed;
    std::shared_ptr<ISubscription> added;
    auto first = signal.subscribe([&](int depth) {
        observed.push_back(10 + depth);
        if (depth == 0) {
            added = signal.subscribe([&](int innerDepth) { observed.push_back(30 + innerDepth); });
            signal.emit(1);
        }
    });
    auto second = signal.subscribe([&](int depth) { observed.push_back(20 + depth); });
    signal.emit(0);
    CHECK((observed == std::vector<int>{10, 11, 21, 31, 20}));
    CHECK(signal.size() == 3);
}

struct DestructorAction {
    std::function<void()> run;
    ~DestructorAction() { if (run) run(); }
};

void capturedDestructorCanReenterSignal()
{
    SignalCOW<> signal;
    int destructors = 0;
    int replacementCalls = 0;
    auto other = signal.subscribe([] {});
    std::shared_ptr<ISubscription> replacement;
    auto captured = std::make_shared<DestructorAction>();
    captured->run = [&] {
        ++destructors;
        replacement = signal.subscribe([&] { ++replacementCalls; });
        other->cancel();
    };
    auto token = signal.subscribe([captured = std::move(captured)] {});
    token->cancel();
    token.reset();
    CHECK(destructors == 1);
    CHECK(signal.size() == 1);
    signal.emit();
    CHECK(replacementCalls == 1);
}

struct MutableCallback {
    std::vector<int>* observed;
    int* copies;
    int counter = 0;

    MutableCallback(std::vector<int>& output, int& count) : observed(&output), copies(&count) {}
    MutableCallback(const MutableCallback& other)
        : observed(other.observed), copies(other.copies), counter(other.counter)
    {
        ++*copies;
    }
    MutableCallback(MutableCallback&&) = default;
    void operator()() { observed->push_back(++counter); }
};

void mutableCallableSurvivesSubscriptionChanges()
{
    SignalCOW<> signal;
    std::vector<int> observed;
    int copies = 0;
    auto token = signal.subscribe(MutableCallback(observed, copies));
    const int initialCopies = copies;
    signal.emit();
    auto other = signal.subscribe([] {});
    CHECK(copies == initialCopies);
    signal.emit();
    other->cancel();
    CHECK(copies == initialCopies);
    signal.emit();
    CHECK((observed == std::vector<int>{1, 2, 3}));
}

void exceptionsPropagateAndStopCurrentEmission()
{
    SignalCOW<> signal;
    int laterCalls = 0;
    auto throwing = signal.subscribe([] { throw std::runtime_error("callback failed"); });
    auto later = signal.subscribe([&] { ++laterCalls; });
    bool caught = false;
    try {
        signal.emit();
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "callback failed";
    }
    CHECK(caught);
    CHECK(laterCalls == 0);
    throwing->cancel();
    signal.emit();
    CHECK(laterCalls == 1);
}

void failedRegistrationDoesNotPublish()
{
    int failures = 0;
    bool reachedSuccessfulRegistration = false;
    // Sweep every allocation in a successful registration without depending
    // on a particular implementation's allocation count or order.
    for (int failureIndex = 0; failureIndex < 32; ++failureIndex) {
        SignalCOW<> signal;
        int existingCalls = 0;
        int newCalls = 0;
        auto existing = signal.subscribe([&] { ++existingCalls; });
        std::shared_ptr<ISubscription> added;
        bool failed = false;
        {
            allocation_failure::FailAfter fail(failureIndex);
            try {
                added = signal.subscribe([&] { ++newCalls; });
            } catch (const std::bad_alloc&) {
                failed = true;
            }
        }
        signal.emit();
        CHECK(existingCalls == 1);
        if (failed) {
            ++failures;
            CHECK(!added);
            CHECK(signal.size() == 1);
            CHECK(newCalls == 0);
        } else {
            CHECK(added);
            CHECK(signal.size() == 2);
            CHECK(newCalls == 1);
            reachedSuccessfulRegistration = true;
            break;
        }
    }
    CHECK(failures > 0);
    CHECK(reachedSuccessfulRegistration);
}

void allocationFailureDuringCancellationStaysCanceled()
{
    SignalCOW<> signal;
    int canceledCalls = 0;
    int remainingCalls = 0;
    auto canceled = signal.subscribe([&] { ++canceledCalls; });
    auto remaining = signal.subscribe([&] { ++remainingCalls; });
    {
        allocation_failure::FailAfter fail(0);
        canceled->cancel();
    }
    CHECK(signal.size() == 1);
    signal.emit();
    CHECK(canceledCalls == 0);
    CHECK(remainingCalls == 1);
    canceled->cancel();
    auto added = signal.subscribe([] {});
    CHECK(signal.size() == 2);
    signal.emit();
    CHECK(canceledCalls == 0);
    CHECK(remainingCalls == 2);

    {
        allocation_failure::FailAfter fail(0);
        remaining.reset();
    }
    CHECK(signal.size() == 1);
    signal.emit();
    CHECK(remainingCalls == 2);
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"RAII and empty subscriptions", raiiAndEmptySubscriptions},
        {"token outlives owner", tokenOutlivesOwner},
        {"concurrent repeated cancellation", concurrentRepeatedCancellation},
        {"nonblocking in-flight cancellation", cancellationDoesNotWaitForRunningCallback},
        {"skip canceled later entry", canceledLaterEntryIsSkipped},
        {"cancel other and self from callback", cancelOtherAndSelfFromCallback},
        {"subscribe and reentrant emit", subscribeAndEmitFromCallback},
        {"captured destructor reenters signal", capturedDestructorCanReenterSignal},
        {"mutable callable identity", mutableCallableSurvivesSubscriptionChanges},
        {"callback exception compatibility", exceptionsPropagateAndStopCurrentEmission},
        {"failed registration is atomic", failedRegistrationDoesNotPublish},
        {"failed removal stays canceled", allocationFailureDuringCancellationStaysCanceled},
    };
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
