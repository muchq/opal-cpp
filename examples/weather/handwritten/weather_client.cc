#include "examples/weather/handwritten/weather_client.h"

#include <utility>

#include "smithy/http/socket_transport.h"
#include "smithy/json/json.h"

namespace example::weather::handwritten {
namespace {

using opal::Document;
using opal::Error;
using opal::Outcome;

// restJson1 error deserialization (prototype): code from the
// x-error-type header (how generated servers send it), else the "__type"
// or "code" body field; message from "message".
Error DeserializeError(const opal::http::HttpResponse& response) {
  std::string code = "UnknownError";
  std::string message = "HTTP " + std::to_string(response.status);
  if (const auto header = response.headers.Get("x-error-type"); header.has_value()) {
    code = *header;
  }
  if (const auto doc = opal::json::Decode(response.body); doc.ok() && doc->is_map()) {
    const Document* type = doc->Find("__type");
    if (type == nullptr) type = doc->Find("code");
    if (code == "UnknownError" && type != nullptr && type->is_string()) code = type->as_string();
    if (const Document* text = doc->Find("message"); text != nullptr && text->is_string()) {
      message = text->as_string();
    }
  }
  const bool retryable = response.status >= 500;
  if (code == "UnknownError") return Error(opal::ErrorKind::kUnknown, code, message, retryable);
  return Error::Modeled(code, message, retryable);
}

template <typename T>
Outcome<T> ParseBody(const opal::http::HttpResponse& response,
                     Outcome<T> (*deserialize)(const Document&)) {
  auto doc = opal::json::Decode(response.body);
  if (!doc) return std::move(doc).error();
  return deserialize(*doc);
}

}  // namespace

opal::Outcome<WeatherClient> WeatherClient::Create(opal::ClientConfig config) {
  std::shared_ptr<opal::http::HttpClient> transport = config.http_client;
  std::string prefix;
  if (!config.endpoint.empty()) {
    auto endpoint = opal::http::ParseEndpoint(config.endpoint);
    if (!endpoint) return std::move(endpoint).error();
    prefix = endpoint->path_prefix;
    if (transport == nullptr) {
      transport = std::make_shared<opal::http::SocketHttpClient>(endpoint->host, endpoint->port,
                                                                 config.request_timeout_ms);
    }
  }
  if (transport == nullptr) {
    return Error::Validation("weather: config needs an endpoint or an http_client");
  }
  return WeatherClient(std::move(config), std::move(transport), std::move(prefix));
}

opal::Outcome<opal::http::HttpResponse> WeatherClient::Send(const std::string& method,
                                                            const std::string& target) const {
  opal::http::HttpRequest request;
  request.method = method;
  request.target = path_prefix_ + target;
  request.headers.Set("accept", "application/json");
  request.headers.Set("user-agent", config_.user_agent);
  return transport_->Send(request);
}

opal::Outcome<GetCityOutput> WeatherClient::GetCity(const GetCityInput& input) const {
  auto response = Send("GET", "/cities/" + opal::http::EncodePathSegment(input.cityId));
  if (!response) return std::move(response).error();
  if (response->status != 200) return DeserializeError(*response);
  return ParseBody(*response, &DeserializeGetCityOutput);
}

opal::Outcome<ListCitiesOutput> WeatherClient::ListCities(const ListCitiesInput& input) const {
  opal::http::QueryString query;
  if (input.nextToken.has_value()) query.Add("nextToken", *input.nextToken);
  if (input.pageSize.has_value()) query.Add("pageSize", std::to_string(*input.pageSize));
  auto response = Send("GET", "/cities" + query.ToString());
  if (!response) return std::move(response).error();
  if (response->status != 200) return DeserializeError(*response);
  return ParseBody(*response, &DeserializeListCitiesOutput);
}

opal::Outcome<GetForecastOutput> WeatherClient::GetForecast(const GetForecastInput& input) const {
  auto response =
      Send("GET", "/cities/" + opal::http::EncodePathSegment(input.cityId) + "/forecast");
  if (!response) return std::move(response).error();
  if (response->status != 200) return DeserializeError(*response);
  return ParseBody(*response, &DeserializeGetForecastOutput);
}

opal::Outcome<GetCurrentTimeOutput> WeatherClient::GetCurrentTime() const {
  auto response = Send("GET", "/current-time");
  if (!response) return std::move(response).error();
  if (response->status != 200) return DeserializeError(*response);
  return ParseBody(*response, &DeserializeGetCurrentTimeOutput);
}

}  // namespace example::weather::handwritten
