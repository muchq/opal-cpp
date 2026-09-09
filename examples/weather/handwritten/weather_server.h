#ifndef OPAL_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_SERVER_H_
#define OPAL_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_SERVER_H_

#include <memory>

#include "examples/weather/handwritten/weather_types.h"
#include "opal/http/transport.h"
#include "opal/server/router.h"

namespace example::weather::handwritten {

// Hand-written mirror of the generated server scaffold (PLAN Phase 4): users
// implement this interface; the service owns routing and serialization.
// Modeled errors are returned as Error::Modeled(kNoSuchResourceCode, ...).
class WeatherHandler {
 public:
  virtual ~WeatherHandler() = default;

  virtual opal::Outcome<GetCityOutput> GetCity(const GetCityInput& input) = 0;
  virtual opal::Outcome<ListCitiesOutput> ListCities(const ListCitiesInput& input) = 0;
  virtual opal::Outcome<GetForecastOutput> GetForecast(const GetForecastInput& input) = 0;
  virtual opal::Outcome<GetCurrentTimeOutput> GetCurrentTime() = 0;
};

// Binds a WeatherHandler to the runtime router. Transport-agnostic: pass
// Handler() to any HttpServerTransport (Loopback, SocketHttpServer, ...).
class WeatherService {
 public:
  // Route registration cannot fail for this fixed, conflict-free route table.
  explicit WeatherService(const std::shared_ptr<WeatherHandler>& handler);

  opal::http::RequestHandler Handler() const;

 private:
  std::shared_ptr<opal::server::Router> router_;
};

}  // namespace example::weather::handwritten

#endif  // OPAL_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_SERVER_H_
