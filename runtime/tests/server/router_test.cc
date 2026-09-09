#include "smithy/server/router.h"

#include <gtest/gtest.h>

namespace opal::server {
namespace {

http::HttpRequest Request(std::string method, std::string target) {
  http::HttpRequest request;
  request.method = std::move(method);
  request.target = std::move(target);
  return request;
}

RouteHandler Tag(const std::string& tag) {
  return [tag](const http::HttpRequest&, const RequestContext& context) {
    http::HttpResponse response;
    response.headers.Set("x-route", tag);
    for (const auto& [name, value] : context.labels) {
      response.headers.Set("x-label-" + name, value);
    }
    return response;
  };
}

TEST(RouterTest, MatchesLiteralAndLabelRoutes) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/cities", Tag("list")).ok());
  ASSERT_TRUE(router.Add("GET", "/cities/{cityId}", Tag("get")).ok());
  ASSERT_TRUE(router.Add("GET", "/cities/{cityId}/forecast", Tag("forecast")).ok());

  EXPECT_EQ(router.Route(Request("GET", "/cities")).headers.Get("x-route"), "list");
  const auto get = router.Route(Request("GET", "/cities/a%20b"));
  EXPECT_EQ(get.headers.Get("x-route"), "get");
  EXPECT_EQ(get.headers.Get("x-label-cityId"), "a b");  // labels arrive decoded
  EXPECT_EQ(router.Route(Request("GET", "/cities/seattle/forecast")).headers.Get("x-route"),
            "forecast");
}

TEST(RouterTest, LiteralOutranksLabel) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/cities/{cityId}", Tag("label")).ok());
  ASSERT_TRUE(router.Add("GET", "/cities/current", Tag("literal")).ok());
  EXPECT_EQ(router.Route(Request("GET", "/cities/current")).headers.Get("x-route"), "literal");
  EXPECT_EQ(router.Route(Request("GET", "/cities/other")).headers.Get("x-route"), "label");
}

TEST(RouterTest, LabelOutranksGreedy) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/files/{path+}", Tag("greedy")).ok());
  ASSERT_TRUE(router.Add("GET", "/files/{name}", Tag("label")).ok());
  EXPECT_EQ(router.Route(Request("GET", "/files/report")).headers.Get("x-route"), "label");
  const auto nested = router.Route(Request("GET", "/files/a/b/c"));
  EXPECT_EQ(nested.headers.Get("x-route"), "greedy");
  EXPECT_EQ(nested.headers.Get("x-label-path"), "a/b/c");
}

TEST(RouterTest, Returns404And405) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/cities", Tag("list")).ok());
  ASSERT_TRUE(router.Add("PUT", "/cities", Tag("put")).ok());

  EXPECT_EQ(router.Route(Request("GET", "/unknown")).status, 404);
  const auto not_allowed = router.Route(Request("DELETE", "/cities"));
  EXPECT_EQ(not_allowed.status, 405);
  const auto allow = not_allowed.headers.Get("allow");
  ASSERT_TRUE(allow.has_value());
  EXPECT_NE(allow->find("GET"), std::string::npos);
  EXPECT_NE(allow->find("PUT"), std::string::npos);
  EXPECT_EQ(router.Route(Request("GET", "/bad%2")).status, 400);
}

TEST(RouterTest, ContextCarriesTheRawRequest) {
  Router router;
  ASSERT_TRUE(
      router
          .Add("GET", "/cities/{cityId}",
               [](const http::HttpRequest& request, const RequestContext& context) {
                 // Route's whole contract here is the pointer wiring: the
                 // context's request IS the routed request (issue #46), so
                 // unmodeled headers and transport annotations follow; one
                 // representative read demonstrates it.
                 http::HttpResponse response;
                 response.headers.Set("x-same-object", context.request == &request ? "yes" : "no");
                 response.headers.Set("x-tenant",
                                      context.request->headers.Get("x-tenant").value_or("missing"));
                 return response;
               })
          .ok());

  http::HttpRequest request = Request("GET", "/cities/rome");
  request.headers.Set("x-tenant", "acme");
  const auto response = router.Route(request);
  EXPECT_EQ(response.headers.Get("x-same-object"), "yes");
  EXPECT_EQ(response.headers.Get("x-tenant"), "acme");
}

TEST(RouterTest, GreedyRefusesALoneEmptySegment) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/files/{path+}", Tag("greedy")).ok());

  // "/files//" reduces to a lone empty segment for {path+}: no match (a
  // greedy label never captures an empty value), while real captures keep
  // working — including ones with embedded empty segments.
  EXPECT_EQ(router.Route(Request("GET", "/files//")).status, 404);
  EXPECT_EQ(router.Route(Request("GET", "/files")).status, 404);
  EXPECT_EQ(router.Route(Request("GET", "/files/a/b")).headers.Get("x-label-path"), "a/b");
}

TEST(RouterTest, AllowListsEachMethodOnceInDeterministicOrder) {
  Router router;
  // Two GET patterns both match /a/b; GET must still appear once, and the
  // list is method-sorted regardless of registration order.
  ASSERT_TRUE(router.Add("PUT", "/a/b", Tag("put")).ok());
  ASSERT_TRUE(router.Add("GET", "/a/{x}", Tag("one")).ok());
  ASSERT_TRUE(router.Add("GET", "/{y}/b", Tag("two")).ok());

  const auto response = router.Route(Request("DELETE", "/a/b"));
  EXPECT_EQ(response.status, 405);
  EXPECT_EQ(response.headers.Get("allow").value_or(""), "GET, PUT");
}

TEST(RouterTest, TrailingSlashesMatch) {
  Router router;
  ASSERT_TRUE(router.Add("GET", "/cities", Tag("list")).ok());
  EXPECT_EQ(router.Route(Request("GET", "/cities/")).headers.Get("x-route"), "list");
}

TEST(RouterTest, QueryParamsReachContext) {
  Router router;
  ASSERT_TRUE(router
                  .Add("GET", "/search",
                       [](const http::HttpRequest&, const RequestContext& context) {
                         http::HttpResponse response;
                         response.body = context.query_params.empty()
                                             ? ""
                                             : context.query_params[0].first + "=" +
                                                   context.query_params[0].second;
                         return response;
                       })
                  .ok());
  EXPECT_EQ(router.Route(Request("GET", "/search?q=a%26b")).body, "q=a&b");
}

TEST(RouterTest, RejectsInvalidAndConflictingPatterns) {
  Router router;
  EXPECT_FALSE(router.Add("GET", "no-slash", Tag("x")).ok());
  EXPECT_FALSE(router.Add("GET", "/a//b", Tag("x")).ok());
  EXPECT_FALSE(router.Add("GET", "/a/{}", Tag("x")).ok());
  EXPECT_FALSE(router.Add("GET", "/{path+}/tail", Tag("x")).ok());

  ASSERT_TRUE(router.Add("GET", "/cities/{cityId}", Tag("a")).ok());
  EXPECT_FALSE(router.Add("GET", "/cities/{different}", Tag("b")).ok());  // same shape
  EXPECT_TRUE(router.Add("PUT", "/cities/{cityId}", Tag("c")).ok());      // other method is fine
}

TEST(RouterTest, RootPatternMatchesRootTarget) {
  Router router;
  ASSERT_TRUE(router.Add("POST", "/", Tag("root")).ok());
  EXPECT_EQ(router.Route(Request("POST", "/")).headers.Get("x-route"), "root");
  EXPECT_EQ(router.Route(Request("POST", "/other")).status, 404);
}

TEST(MakeErrorResponseTest, ProducesJsonBody) {
  const auto response = MakeErrorResponse(404, "NotFound", "no such city");
  EXPECT_EQ(response.status, 404);
  EXPECT_EQ(response.headers.Get("content-type"), "application/json");
  EXPECT_EQ(response.body, R"({"code":"NotFound","message":"no such city"})");
}

}  // namespace
}  // namespace opal::server
