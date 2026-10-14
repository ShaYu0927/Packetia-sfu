#include "server/api/ApiRouter.h"
#include "server/api/ApiResponse.h"

#include <gtest/gtest.h>
#include <future>
#include <stdexcept>
#include <vector>

namespace
{
using namespace server::api;
using protocol::http::HttpRequest;
using protocol::http::HttpResponse;

HttpRequest JsonRequest(std::string path, std::string body)
{
    return {"POST", std::move(path), std::move(body), {{"content-type", "application/json"}}};
}

TEST(ApiRouter, DistinguishesMethodsAndReportsAllAllowedMethods)
{
    ApiRouter::Builder builder;
    builder.Get("/resource", [](const ApiRequest&) { return JsonResponse(200, {{"method", "GET"}}); });
    builder.PostJson("/resource", [](const ApiRequest& request) { return JsonResponse(201, request.json); });
    builder.Delete("/resource", [](const ApiRequest&) { return HttpResponse{204, {}, {}}; });
    const auto router = builder.Build();
    EXPECT_EQ(router.Dispatch({"GET", "/resource", {}, {}}).status, 200);
    EXPECT_EQ(router.Dispatch(JsonRequest("/resource", R"({"value":1})")).status, 201);
    EXPECT_EQ(router.Dispatch({"DELETE", "/resource", {}, {}}).status, 204);
    const auto wrong_method = router.Dispatch({"PATCH", "/resource", {}, {}});
    EXPECT_EQ(wrong_method.status, 405);
    EXPECT_EQ(wrong_method.headers.at("allow"), "DELETE, GET, POST");
    EXPECT_EQ(router.Dispatch({"GET", "/missing", {}, {}}).status, 404);
    EXPECT_EQ(router.Dispatch({"GET", "/RESOURCE", {}, {}}).status, 404);
}

TEST(ApiRouter, RejectsDuplicateRoutesAndInvalidRegistrations)
{
    const auto handler = [](const ApiRequest&) { return HttpResponse{200, {}, {}}; };
    ApiRouter::Builder builder;
    builder.Get("/resource", handler);
    EXPECT_THROW(builder.Get("/resource", handler), std::logic_error);
    EXPECT_NO_THROW(builder.Post("/resource", handler));
    EXPECT_THROW(builder.Add("get", "/other", handler), std::invalid_argument);
    EXPECT_THROW(builder.Get("relative", handler), std::invalid_argument);
    EXPECT_THROW(builder.Get("/resource?query=x", handler), std::invalid_argument);
    EXPECT_THROW(builder.Get("/bad\r\npath", handler), std::invalid_argument);
    EXPECT_THROW(builder.Get("/empty", {}), std::invalid_argument);
    EXPECT_THROW(builder.Build(0), std::invalid_argument);
}

TEST(ApiRouter, ValidatesJsonBeforeCallingBusinessHandler)
{
    int invoked = 0;
    ApiRouter::Builder builder;
    builder.PostJson("/json", [&](const ApiRequest& request) {
        ++invoked;
        return JsonResponse(200, request.json);
    });
    const auto router = builder.Build(128);
    EXPECT_EQ(router.Dispatch(JsonRequest("/json", "[")).status, 400);
    EXPECT_EQ(router.Dispatch(JsonRequest("/json", "[]")).status, 400);
    EXPECT_EQ(router.Dispatch(JsonRequest("/json", R"({"x":1,"x":2})")).status, 400);
    EXPECT_EQ(router.Dispatch(JsonRequest("/json", R"({"outer":{"x":1,"x":2}})")).status, 400);
    EXPECT_EQ(router.Dispatch(JsonRequest("/json", std::string(129, 'x'))).status, 413);
    EXPECT_EQ(router.Dispatch({"POST", "/json", "{}", {}}).status, 415);
    EXPECT_EQ(invoked, 0);
    auto valid = JsonRequest("/json", R"({"x":1})");
    valid.headers["content-type"] = "Application/JSON; charset=utf-8";
    const auto response = router.Dispatch(valid);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("x"), 1);
    EXPECT_EQ(invoked, 1);
}

TEST(ApiRouter, EnforcesEmptyBodyRoutesAndPreservesRawBodies)
{
    ApiRouter::Builder builder;
    builder.Get("/empty", [](const ApiRequest&) { return HttpResponse{200, {}, {}}; });
    builder.Post("/raw", [](const ApiRequest& request) {
        return HttpResponse{200, request.http.body, {}};
    });
    const auto router = builder.Build();
    EXPECT_EQ(router.Dispatch({"GET", "/empty", "unexpected", {}}).status, 400);
    EXPECT_EQ(router.Dispatch({"POST", "/raw", "raw body", {}}).body, "raw body");
}

TEST(ApiRouter, ConvertsUnhandledExceptionsWithoutExposingDetails)
{
    ApiRouter::Builder builder;
    builder.Get("/failure", [](const ApiRequest&) -> HttpResponse {
        throw std::runtime_error("database password secret");
    });
    const auto response = builder.Build().Dispatch({"GET", "/failure", {}, {}});
    EXPECT_EQ(response.status, 500);
    EXPECT_EQ(response.headers.at("content-type"), "application/json; charset=utf-8");
    EXPECT_EQ(response.body.find("secret"), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(response.body)["error"]["code"], "internal_error");
}

TEST(ApiRouter, BuiltSnapshotRemainsIndependentDuringConcurrentDispatch)
{
    ApiRouter::Builder builder;
    builder.Get("/existing", [](const ApiRequest&) { return JsonResponse(200, {{"value", 7}}); });
    const auto first = builder.Build();
    builder.Get("/later", [](const ApiRequest&) { return HttpResponse{204, {}, {}}; });
    const auto second = builder.Build();
    EXPECT_EQ(first.Dispatch({"GET", "/later", {}, {}}).status, 404);
    EXPECT_EQ(second.Dispatch({"GET", "/later", {}, {}}).status, 204);
    std::vector<std::future<bool>> readers;
    for (int i = 0; i < 8; ++i)
        readers.push_back(std::async(std::launch::async, [&] {
            for (int j = 0; j < 32; ++j)
            {
                const auto response = first.Dispatch({"GET", "/existing", {}, {}});
                if (response.status != 200 || nlohmann::json::parse(response.body)["value"] != 7) return false;
            }
            return true;
        }));
    for (auto& reader : readers) EXPECT_TRUE(reader.get());
}
}
