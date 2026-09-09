// Fuzz target: full generated-server dispatch (router + bindings + serde +
// validation) fed raw method/target/headers/body. Must never crash and must
// always produce a response; the malformed-request corpus's job, unbounded.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "example/weather/server.h"
#include "smithy/http/message.h"

namespace {

class NullHandler final : public example::weather::WeatherHandler {
 public:
  opal::Outcome<example::weather::GetCityOutput> GetCity(
      const example::weather::GetCityInput&, const opal::server::RequestContext&) override {
    return example::weather::GetCityOutput{.name = "x"};
  }
  opal::Outcome<example::weather::DeleteCityOutput> DeleteCity(
      const example::weather::DeleteCityInput&, const opal::server::RequestContext&) override {
    return example::weather::DeleteCityOutput{};
  }
  opal::Outcome<example::weather::ListCitiesOutput> ListCities(
      const example::weather::ListCitiesInput&, const opal::server::RequestContext&) override {
    return example::weather::ListCitiesOutput{};
  }
  opal::Outcome<example::weather::GetForecastOutput> GetForecast(
      const example::weather::GetForecastInput&, const opal::server::RequestContext&) override {
    return example::weather::GetForecastOutput{};
  }
  opal::Outcome<example::weather::GetCurrentTimeOutput> GetCurrentTime(
      const example::weather::GetCurrentTimeInput&, const opal::server::RequestContext&) override {
    return example::weather::GetCurrentTimeOutput{};
  }
  opal::Outcome<example::weather::GetReportOutput> GetReport(
      const example::weather::GetReportInput& input, const opal::server::RequestContext&) override {
    return example::weather::GetReportOutput{.path = input.reportPath, .sizeBytes = 0};
  }
};

opal::http::RequestHandler& Handler() {
  static auto* server = new example::weather::WeatherServer(std::make_shared<NullHandler>());
  static auto* handler = new opal::http::RequestHandler(server->Handler());
  return *handler;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Layout: method '\n' target '\n' header-value '\n' body.
  const std::string all(reinterpret_cast<const char*>(data), size);
  opal::http::HttpRequest request;
  std::size_t start = 0;
  std::string* fields[] = {&request.method, &request.target};
  for (std::string* field : fields) {
    const auto newline = all.find('\n', start);
    if (newline == std::string::npos) break;
    *field = all.substr(start, newline - start);
    start = newline + 1;
  }
  const auto newline = all.find('\n', start);
  if (newline != std::string::npos) {
    request.headers.Set("content-type", all.substr(start, newline - start));
    request.body = all.substr(newline + 1);
  }
  const auto response = Handler()(request);
  if (response.status < 100 || response.status > 599) std::abort();
  return 0;
}
