#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ShardedWorkerPool.h"
#include "EndpointBase.h"
#include "MediaStreamAffinity.h"

namespace utils
{

enum : uint32_t
{
    kWorkTest = 1,
    kWorkClose = 2,
};

class TestEndpoint : public EndpointBase
{
public:
    TestEndpoint(std::uint64_t endpoint_id, std::string name)
        : EndpointBase(endpoint_id, std::move(name))
    {
    }

    bool Start() override
    {
        SetState(State::kRunning);
        return true;
    }

    void Stop() override
    {
        SetState(State::kStopped);
    }

    void ProcessJob(WorkJob& job) override
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            handled_types_.push_back(static_cast<uint32_t>(job.type));
            ++handled_count_;
        }

        cv_.notify_all();

        if (job.type == WorkType::Invalid)
        {
            Stop();
        }

        if (job.deleter)
        {
            job.deleter(job);
        }
    }

    bool WaitHandledCount(std::size_t expected,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
    {
        std::unique_lock<std::mutex> lock(mtx_);
        return cv_.wait_for(lock, timeout, [&] {
            return handled_count_ >= expected;
        });
    }

    std::size_t HandledCount() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return handled_count_;
    }

    std::vector<uint32_t> HandledTypes() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return handled_types_;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::size_t handled_count_{0};
    std::vector<uint32_t> handled_types_;
};

class GlobalPoolGuard
{
public:
    explicit GlobalPoolGuard(std::string name) : name_(std::move(name))
    {
        WorkerService::destroy_pool(name_, false);
    }

    ~GlobalPoolGuard()
    {
        WorkerService::destroy_pool(name_, true);
    }

private:
    std::string name_;
};

TEST(WorkerServiceGlobalTest, SupportsDynamicNamedFunctionPoolsAndAffinity)
{
    constexpr const char* kPoolName = "Recording_Test";
    GlobalPoolGuard guard(kPoolName);

    ASSERT_EQ(0, WorkerService::create_function_pool(kPoolName, 2, 64));
    EXPECT_TRUE(WorkerService::exists("recording_test"));

    std::mutex values_mutex;
    std::vector<int> values;
    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();

    for (int value = 0; value < 3; ++value)
    {
        ASSERT_EQ(0, WorkerService::post_fn(
                         kPoolName, 9527,
                         [&, value, completed] {
                             {
                                 std::lock_guard<std::mutex> lock(values_mutex);
                                 values.push_back(value);
                             }
                             if (value == 2)
                             {
                                 completed->set_value();
                             }
                         }));
    }

    ASSERT_EQ(std::future_status::ready,
              future.wait_for(std::chrono::seconds(2)));
    EXPECT_EQ((std::vector<int>{0, 1, 2}), values);

    const auto statuses = WorkerService::status();
    auto status = std::find_if(
        statuses.begin(), statuses.end(),
        [](const WorkerService::PoolStatus& item) {
            return item.name == "recording_test";
        });
    ASSERT_NE(statuses.end(), status);
    EXPECT_EQ(2u, status->stats.worker_count);
    EXPECT_EQ(3u, status->stats.enqueued);
    EXPECT_EQ(3u, status->stats.dequeued);
}

TEST(WorkerServiceGlobalTest, FunctionExceptionDoesNotStopWorker)
{
    constexpr const char* kPoolName = "transcode_test";
    GlobalPoolGuard guard(kPoolName);

    ASSERT_EQ(0, WorkerService::create_function_pool(kPoolName, 1, 16));

    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();
    ASSERT_EQ(0, WorkerService::post_fn(kPoolName, 1, [] {
        throw std::runtime_error("expected test exception");
    }));
    ASSERT_EQ(0, WorkerService::post_fn(kPoolName, 1, [completed] {
        completed->set_value();
    }));

    EXPECT_EQ(std::future_status::ready,
              future.wait_for(std::chrono::seconds(2)));
}

TEST(MediaStreamAffinityTest, StreamHandleIsStableAndSeparatesEndpoints)
{
    const auto first = media_affinity::MakeStreamHandle(100, 0x11223344);
    const auto again = media_affinity::MakeStreamHandle(100, 0x11223344);
    const auto other_endpoint =
        media_affinity::MakeStreamHandle(101, 0x11223344);

    EXPECT_EQ(first.endpoint_id, 100u);
    EXPECT_EQ(first.ssrc, 0x11223344u);
    EXPECT_EQ(first.affinity_key, again.affinity_key);
    EXPECT_NE(first.affinity_key, other_endpoint.affinity_key);
}

TEST(MediaStreamAffinityTest, UnifiedPostPreservesStreamOrderAndOwnerThread)
{
    GlobalPoolGuard guard("media");
    ASSERT_EQ(0, WorkerService::create_function_pool("media", 4, 64));

    std::mutex mutex;
    std::vector<int> values;
    std::vector<std::thread::id> threads;
    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();

    for (int value = 0; value < 4; ++value)
    {
        ASSERT_EQ(0, media_affinity::PostToMediaStream(
                         77, 0xaabbccdd,
                         [&, value, completed] {
                             {
                                 std::lock_guard<std::mutex> lock(mutex);
                                 values.push_back(value);
                                 threads.push_back(std::this_thread::get_id());
                             }
                             if (value == 3)
                             {
                                 completed->set_value();
                             }
                         }));
    }

    ASSERT_EQ(std::future_status::ready,
              future.wait_for(std::chrono::seconds(2)));
    EXPECT_EQ((std::vector<int>{0, 1, 2, 3}), values);
    ASSERT_EQ(threads.size(), 4u);
    EXPECT_TRUE(std::all_of(threads.begin(), threads.end(),
                            [&](std::thread::id id) {
                                return id == threads.front();
                            }));
}

// class ShardedWorkerPoolTest : public ::testing::Test
// {
// protected:
//     void SetUp() override
//     {
//         handler_ = std::make_shared<EndpointJobHandler>(&endpoint_mgr_);
//         ASSERT_EQ(pool_.start(4, handler_, 1024, ShardedWorkerPool::DropPolicy::DropHead), 0);
//     }

//     void TearDown() override
//     {
//         pool_.shutdown(true);
//     }

//     std::shared_ptr<TestEndpoint> CreateEndpoint(const std::string& name)
//     {
//         auto id = endpoint_mgr_.AllocId();
//         auto ep = std::make_shared<TestEndpoint>(id, name);
//         EXPECT_TRUE(ep->Start());
//         EXPECT_TRUE(endpoint_mgr_.Add(ep));
//         return ep;
//     }

//     WorkJob MakeJob(std::uint64_t endpoint_id, std::uint32_t type,
//                     std::atomic<int>* release_counter = nullptr)
//     {
//         WorkJob job;
//         // job.key = endpoint_id;
//         // job.type = type;
//         // job.payload_len = 0;
//         // job.enqueue_ts = 0;
//         // job.deleter = [release_counter](WorkJob&) {
//         //     if (release_counter)
//         //     {
//         //         ++(*release_counter);
//         //     }
//         // };
//         return job;
//     }

// protected:
//     EndpointManager endpoint_mgr_;
//     std::shared_ptr<EndpointJobHandler> handler_;
//     ShardedWorkerPool pool_;
// };

// TEST_F(ShardedWorkerPoolTest, PostShouldDispatchToEndpointAsynchronously)
// {
//     auto ep = CreateEndpoint("ep_async");

//     auto job = MakeJob(ep->Id(), kWorkTest);
//     ASSERT_EQ(pool_.post(std::move(job)), 0);

//     EXPECT_TRUE(ep->WaitHandledCount(1));
//     EXPECT_EQ(ep->HandledCount(), 1u);
// }

// TEST_F(ShardedWorkerPoolTest, SameEndpointJobsShouldKeepOrder)
// {
//     auto ep = CreateEndpoint("ep_order");

//     constexpr int kCount = 10;
//     for (int i = 0; i < kCount; ++i)
//     {
//         auto job = MakeJob(ep->Id(), static_cast<uint32_t>(100 + i));
//         ASSERT_EQ(pool_.post(std::move(job)), 0);
//     }

//     EXPECT_TRUE(ep->WaitHandledCount(kCount));

//     auto types = ep->HandledTypes();
//     ASSERT_EQ(types.size(), static_cast<std::size_t>(kCount));
//     for (int i = 0; i < kCount; ++i)
//     {
//         EXPECT_EQ(types[i], static_cast<uint32_t>(100 + i));
//     }
// }

// TEST_F(ShardedWorkerPoolTest, MissingEndpointShouldReleaseJob)
// {
//     std::atomic<int> release_count{0};

//     auto job = MakeJob(999999, kWorkTest, &release_count);
//     ASSERT_EQ(pool_.post(std::move(job)), 0);

//     auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
//     while (release_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
//     {
//         std::this_thread::sleep_for(std::chrono::milliseconds(10));
//     }

//     EXPECT_EQ(release_count.load(), 1);
// }

// TEST_F(ShardedWorkerPoolTest, DifferentEndpointsShouldBothReceiveJobs)
// {
//     auto ep1 = CreateEndpoint("ep1");
//     auto ep2 = CreateEndpoint("ep2");

//     ASSERT_EQ(pool_.post(MakeJob(ep1->Id(), 1)), 0);
//     ASSERT_EQ(pool_.post(MakeJob(ep2->Id(), 2)), 0);
//     ASSERT_EQ(pool_.post(MakeJob(ep1->Id(), 3)), 0);
//     ASSERT_EQ(pool_.post(MakeJob(ep2->Id(), 4)), 0);

//     EXPECT_TRUE(ep1->WaitHandledCount(2));
//     EXPECT_TRUE(ep2->WaitHandledCount(2));

//     EXPECT_EQ(ep1->HandledCount(), 2u);
//     EXPECT_EQ(ep2->HandledCount(), 2u);

//     auto t1 = ep1->HandledTypes();
//     auto t2 = ep2->HandledTypes();

//     ASSERT_EQ(t1.size(), 2u);
//     ASSERT_EQ(t2.size(), 2u);

//     EXPECT_EQ(t1[0], 1u);
//     EXPECT_EQ(t1[1], 3u);
//     EXPECT_EQ(t2[0], 2u);
//     EXPECT_EQ(t2[1], 4u);
// }
}  // namespace
