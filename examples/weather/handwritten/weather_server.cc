#include "examples/weather/handwritten/weather_server.h"

#include <cstdlib>
#include <string>
#include <utility>

#include "smithy/json/json.h"

namespace example::weather::handwritten {
namespace {

using opal::Document;
using opal::Error;
using opal::ErrorKind;
using opal::http::HttpRequest;
using opal::http::HttpResponse;
using opal::server::MakeErrorResponse;
using opal::server::RequestContext;

HttpResponse ErrorToResponse(const Error& error) {
  if (error.kind() == ErrorKind::kModeled && error.code() == kNoSuchResourceCode) {
    HttpResponse response;
    response.status = 404;  // @httpError(404) on NoSuchResource
    response.headers.Set("content-type", "application/json");
    opal::DocumentMap body;
    body.emplace("__type", Document(error.code()));
    body.emplace("message", Document(error.message()));
    response.body = opal::json::Encode(Document(std::move(body)));
    return response;
  }
  if (error.kind() == ErrorKind::kValidation || error.kind() == ErrorKind::kSerialization) {
    return MakeErrorResponse(400, "ValidationException", error.message());
  }
  // Never leak internal detail on unexpected failures.
  return MakeErrorResponse(500, "InternalFailure", "internal failure");
}

HttpResponse JsonResponse(const Document& doc) {
  HttpResponse response;
  response.headers.Set("content-type", "application/json");
  response.body = opal::json::Encode(doc);
  return response;
}

}  // namespace

WeatherService::WeatherService(const std::shared_ptr<WeatherHandler>& handler)
    : router_(std::make_shared<opal::server::Router>()) {
  // The route table below is conflict-free by construction, so Add cannot
  // fail; the generator emits the same table from the @http traits.
  (void)router_->Add("GET", "/cities/{cityId}",
                     [handler](const HttpRequest&, const RequestContext& context) {
                       GetCityInput input;
                       input.cityId = context.labels.at("cityId");
                       auto outcome = handler->GetCity(input);
                       if (!outcome) return ErrorToResponse(outcome.error());
                       return JsonResponse(SerializeGetCityOutput(*outcome));
                     });
  (void)router_->Add("GET", "/cities",
                     [handler](const HttpRequest&, const RequestContext& context) {
                       ListCitiesInput input;
                       for (const auto& [key, value] : context.query_params) {
                         if (key == "nextToken") input.nextToken = value;
                         if (key == "pageSize") input.pageSize = std::atoi(value.c_str());
                       }
                       auto outcome = handler->ListCities(input);
                       if (!outcome) return ErrorToResponse(outcome.error());
                       return JsonResponse(SerializeListCitiesOutput(*outcome));
                     });
  (void)router_->Add("GET", "/cities/{cityId}/forecast",
                     [handler](const HttpRequest&, const RequestContext& context) {
                       GetForecastInput input;
                       input.cityId = context.labels.at("cityId");
                       auto outcome = handler->GetForecast(input);
                       if (!outcome) return ErrorToResponse(outcome.error());
                       return JsonResponse(SerializeGetForecastOutput(*outcome));
                     });
  (void)router_->Add("GET", "/current-time", [handler](const HttpRequest&, const RequestContext&) {
    auto outcome = handler->GetCurrentTime();
    if (!outcome) return ErrorToResponse(outcome.error());
    return JsonResponse(SerializeGetCurrentTimeOutput(*outcome));
  });
}

opal::http::RequestHandler WeatherService::Handler() const {
  auto router = router_;
  return [router](const HttpRequest& request) { return router->Route(request); };
}

}  // namespace example::weather::handwritten
