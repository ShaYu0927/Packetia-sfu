#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ShardedWorkerPool.h"
#include "EndpointBase.h"

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