#include "server/api/AccountController.h"
#include "server/api/ApiServer.h"
#include "network/http/LibwebsocketsHttpServer.h"
#include "service/Account/AccountService.h"

#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>

namespace
{

using network::http::HttpRequest;
using network::http::HttpResponse;
using namespace std::chrono_literals;

HttpRequest Login(std::string body)
{
    return {"POST", "/api/v1/sessions", std::move(body), {{"content-type", "application/json"}}};
}

std::shared_ptr<service::account::AccountService> UnconfiguredAccounts()
{
    return std::make_shared<service::account::AccountService>(
        service::account::AccountOptions{}, nullptr, nullptr);
}

class FakeHttpServer final : public network::http::HttpServer
{
public:
    bool Start(const network::http::HttpOptions&, RequestHandler callback) override
    {
        ++starts;
        handler = std::move(callback);
        return true;
    }
    void Stop() noexcept override { ++stops; handler = {}; }

    RequestHandler handler;
    int starts = 0;
    int stops = 0;
};

TEST(AccountApiFramework, RejectsAmbiguousAndPrivilegeBearingLoginInput)
{
    server::api::AccountController controller(UnconfiguredAccounts(), 256);
    EXPECT_EQ(controller.Handle(Login(R"({"login":"alice","password":"secret","role":"admin"})")).status, 400);
    EXPECT_EQ(controller.Handle(Login(R"({"login":"alice","login":"admin","password":"secret"})")).status, 400);
    EXPECT_EQ(controller.Handle(Login(R"({"login":"alice","password":true})")).status, 400);
    EXPECT_EQ(controller.Handle(Login("[")).status, 400);
    EXPECT_EQ(controller.Handle(Login(std::string(257, 'x'))).status, 413);
    auto wrong_type = Login(R"({"login":"alice","password":"secret"})");
    wrong_type.headers.clear();
    EXPECT_EQ(controller.Handle(wrong_type).status, 415);
}

TEST(AccountApiFramework, NeverAuthenticatesOrRegistersThroughUnconfiguredDependencies)
{
    server::api::AccountController controller(UnconfiguredAccounts());
    auto login = Login(R"({"login":"alice","password":"secret"})");
    const auto response = controller.Handle(login);
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(response.headers.at("cache-control"), "no-store");
    EXPECT_EQ(response.body.find("session_token"), std::string::npos);

    login.path = "/api/v1/accounts";
    EXPECT_EQ(controller.Handle(login).status, 501);
    EXPECT_EQ(controller.Handle({"GET", "/api/v1/me", {}, {}}).status, 401);
    EXPECT_EQ(controller.Handle({"GET", "/api/v1/me", {}, {{"authorization", "Bearer test"}}}).status, 503);
    EXPECT_EQ(controller.Handle({"GET", "/api/v1/me", {}, {{"authorization", "Bearer test extra"}}}).status, 401);
    const auto method = controller.Handle({"POST", "/api/v1/me", {}, {}});
    EXPECT_EQ(method.status, 405);
    EXPECT_EQ(method.headers.at("allow"), "GET");
}

TEST(AccountApiFramework, UnimplementedListenerRollsBackWorkerPool)
{
    const auto previous_pools = WorkerService::pool_names();
    network::http::HttpOptions http;
    http.enabled = true;
    service::account::AccountOptions account;
    account.enabled = true;
    server::api::ApiServer api(http, account, UnconfiguredAccounts(),
                               std::make_shared<network::http::LibwebsocketsHttpServer>());
    EXPECT_FALSE(api.Start());
    EXPECT_EQ(WorkerService::pool_names(), previous_pools);
    api.Stop();
    EXPECT_EQ(WorkerService::pool_names(), previous_pools);
}

TEST(AccountApiFramework, RunsOnWorkerAndRejectsRetiredConnectionCallbacks)
{
    network::http::HttpOptions http;
    http.enabled = true;
    service::account::AccountOptions account;
    account.enabled = true;
    const auto transport = std::make_shared<FakeHttpServer>();
    server::api::ApiServer api(http, account, UnconfiguredAccounts(), transport);
    ASSERT_TRUE(api.Start());
    ASSERT_TRUE(api.Start());
    EXPECT_EQ(transport->starts, 1);
    const auto old_handler = transport->handler;
    const auto caller = std::this_thread::get_id();
    std::promise<std::pair<std::uint16_t, std::thread::id>> result;
    auto future = result.get_future();
    old_handler({"GET", "/unknown", {}, {}}, [&](HttpResponse response)
    {
        result.set_value({response.status, std::this_thread::get_id()});
    });
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const auto completed = future.get();
    EXPECT_EQ(completed.first, 404);
    EXPECT_NE(completed.second, caller);
    api.Stop();
    EXPECT_EQ(transport->stops, 1);

    ASSERT_TRUE(api.Start());
    std::uint16_t late_status = 0;
    old_handler({"GET", "/unknown", {}, {}}, [&](HttpResponse response) { late_status = response.status; });
    EXPECT_EQ(late_status, 503);
    api.Stop();
}

TEST(AccountApiFramework, AppliesOneAdmissionBudgetAcrossAllWorkerShards)
{
    network::http::HttpOptions http;
    http.enabled = true;
    service::account::AccountOptions account;
    account.enabled = true;
    account.worker_count = 2;
    account.max_pending_requests = 1;
    const auto transport = std::make_shared<FakeHttpServer>();
    server::api::ApiServer api(http, account, UnconfiguredAccounts(), transport);
    ASSERT_TRUE(api.Start());

    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto released = release.get_future().share();
    transport->handler({"GET", "/unknown", {}, {}}, [&](HttpResponse)
    {
        entered.set_value();
        released.wait();
    });
    if (entered_future.wait_for(2s) != std::future_status::ready)
    {
        release.set_value();
        api.Stop();
        FAIL() << "API worker did not receive the first request";
    }
    std::uint16_t overflow = 0;
    transport->handler({"GET", "/unknown", {}, {}}, [&](HttpResponse response) { overflow = response.status; });
    EXPECT_EQ(overflow, 503);
    release.set_value();
    api.Stop();
}

}
