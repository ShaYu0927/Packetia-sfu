#include "server/api/AccountController.h"
#include "server/api/ApiServer.h"
#include "protocol/http/HttpServer.h"
#include "service/Account/AccountService.h"

#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace
{

using protocol::http::HttpRequest;
using protocol::http::HttpResponse;
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


server::api::ApiRouter AccountRoutes(std::size_t max_body_bytes = 8192)
{
    server::api::ApiRouter::Builder builder;
    server::api::AccountController(UnconfiguredAccounts()).RegisterRoutes(builder);
    return builder.Build(max_body_bytes);
}

class FakeHttpServer final : public protocol::http::IHttpServer
{
public:
    bool Start(const protocol::http::HttpServerOptions&, RequestHandler callback) override
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
    const auto router = AccountRoutes(256);
    EXPECT_EQ(router.Dispatch(Login(R"({"login":"alice","password":"secret","role":"admin"})")).status, 400);
    EXPECT_EQ(router.Dispatch(Login(R"({"login":"alice","login":"admin","password":"secret"})")).status, 400);
    EXPECT_EQ(router.Dispatch(Login(R"({"login":"alice","password":true})")).status, 400);
    EXPECT_EQ(router.Dispatch(Login("[")).status, 400);
    EXPECT_EQ(router.Dispatch(Login(std::string(257, 'x'))).status, 413);
    auto wrong_type = Login(R"({"login":"alice","password":"secret"})");
    wrong_type.headers.clear();
    EXPECT_EQ(router.Dispatch(wrong_type).status, 415);
}

TEST(AccountApiFramework, NeverAuthenticatesOrRegistersThroughUnconfiguredDependencies)
{
    const auto router = AccountRoutes();
    auto login = Login(R"({"login":"alice","password":"secret"})");
    const auto response = router.Dispatch(login);
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(response.headers.at("cache-control"), "no-store");
    EXPECT_EQ(response.body.find("session_token"), std::string::npos);

    login.path = "/api/v1/accounts";
    EXPECT_EQ(router.Dispatch(login).status, 501);
    EXPECT_EQ(router.Dispatch({"GET", "/api/v1/me", {}, {}}).status, 401);
    EXPECT_EQ(router.Dispatch({"GET", "/api/v1/me", {}, {{"authorization", "Bearer test"}}}).status, 503);
    EXPECT_EQ(router.Dispatch({"GET", "/api/v1/me", {}, {{"authorization", "Bearer test extra"}}}).status, 401);
    const auto method = router.Dispatch({"POST", "/api/v1/me", {}, {}});
    EXPECT_EQ(method.status, 405);
    EXPECT_EQ(method.headers.at("allow"), "GET");
}

TEST(AccountApiFramework, MissingIoOwnerRollsBackWorkerPool)
{
    const auto previous_pools = WorkerService::pool_names();
    protocol::http::HttpServerOptions http;
    http.enabled = true;
    service::account::AccountOptions account;
    account.enabled = true;
    server::api::ApiServer api(http, account, UnconfiguredAccounts(),
                               std::make_shared<protocol::http::HttpServer>());
    EXPECT_FALSE(api.Start());
    EXPECT_EQ(WorkerService::pool_names(), previous_pools);
    api.Stop();
    EXPECT_EQ(WorkerService::pool_names(), previous_pools);
}

TEST(AccountApiFramework, RunsOnWorkerAndRejectsRetiredConnectionCallbacks)
{
    protocol::http::HttpServerOptions http;
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

TEST(AccountApiFramework, DispatchesRegisteredAccountRoutesThroughWorkers)
{
    protocol::http::HttpServerOptions http;
    http.enabled = true;
    service::account::AccountOptions account;
    account.enabled = true;
    const auto transport = std::make_shared<FakeHttpServer>();
    server::api::ApiServer api(http, account, UnconfiguredAccounts(), transport);
    ASSERT_TRUE(api.Start());
    const std::vector<std::pair<HttpRequest, std::uint16_t>> cases{
        {Login(R"({"login":"alice","password":"secret"})"), 503},
        {{"POST", "/api/v1/accounts", R"({"login":"alice","password":"secret"})",
          {{"content-type", "application/json"}}}, 501},
        {{"GET", "/api/v1/me", {}, {}}, 401},
        {{"POST", "/api/v1/sessions/renew", {}, {}}, 401},
        {{"DELETE", "/api/v1/sessions/current", {}, {}}, 401},
        {{"GET", "/api/v1/sessions", {}, {}}, 405}
    };
    const auto caller = std::this_thread::get_id();
    for (const auto& item : cases)
    {
        auto result = std::make_shared<std::promise<std::pair<HttpResponse, std::thread::id>>>();
        auto future = result->get_future();
        transport->handler(item.first, [result](HttpResponse response) {
            result->set_value({std::move(response), std::this_thread::get_id()});
        });
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        const auto response = future.get();
        EXPECT_EQ(response.first.status, item.second) << item.first.method << ' ' << item.first.path;
        EXPECT_EQ(response.first.headers.at("content-type"), "application/json; charset=utf-8");
        EXPECT_NE(response.second, caller);
    }
    api.Stop();
}

TEST(AccountApiFramework, AppliesOneAdmissionBudgetAcrossAllWorkerShards)
{
    protocol::http::HttpServerOptions http;
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
