#ifndef SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_CLIENT_H_
#define SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_CLIENT_H_

#include <memory>
#include <string>

#include "examples/weather/handwritten/weather_types.h"
#include "smithy/client/config.h"
#include "smithy/core/outcome.h"
#include "smithy/http/uri.h"

namespace example::weather::handwritten {

// Hand-written mirror of the generated restJson1 client (PLAN Phase 3).
// Modeled errors surface as Error with kind kModeled and code() equal to the
// error shape name (e.g. "NoSuchResource").
class WeatherClient {
 public:
  // Fails when the endpoint cannot be parsed and no transport is injected.
  static opal::Outcome<WeatherClient> Create(opal::ClientConfig config);

  opal::Outcome<GetCityOutput> GetCity(const GetCityInput& input) const;
  opal::Outcome<ListCitiesOutput> ListCities(const ListCitiesInput& input) const;
  opal::Outcome<GetForecastOutput> GetForecast(const GetForecastInput& input) const;
  opal::Outcome<GetCurrentTimeOutput> GetCurrentTime() const;

 private:
  WeatherClient(opal::ClientConfig config, std::shared_ptr<opal::http::HttpClient> transport,
                std::string path_prefix)
      : config_(std::move(config)),
        transport_(std::move(transport)),
        path_prefix_(std::move(path_prefix)) {}

  opal::Outcome<opal::http::HttpResponse> Send(const std::string& method,
                                               const std::string& target) const;

  opal::ClientConfig config_;
  std::shared_ptr<opal::http::HttpClient> transport_;
  std::string path_prefix_;
};

}  // namespace example::weather::handwritten

#endif  // SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_CLIENT_H_
