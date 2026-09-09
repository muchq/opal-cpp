// Hand-written mirror of what the Phase 2/3 generators will emit for
// examples/weather/model/weather.smithy. This code is the design prototype
// the generated output must reproduce (PLAN Phase 1); keep it boring.

#ifndef SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_TYPES_H_
#define SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_TYPES_H_

#include <optional>
#include <string>
#include <vector>

#include "smithy/core/document.h"
#include "smithy/core/outcome.h"

namespace example::weather::handwritten {

struct CityCoordinates {
  double latitude = 0;
  double longitude = 0;

  friend bool operator==(const CityCoordinates&, const CityCoordinates&) = default;
};

struct GetCityInput {
  std::string cityId;
};

struct GetCityOutput {
  std::string name;
  CityCoordinates coordinates;

  friend bool operator==(const GetCityOutput&, const GetCityOutput&) = default;
};

struct CitySummary {
  std::string cityId;
  std::string name;

  friend bool operator==(const CitySummary&, const CitySummary&) = default;
};

struct ListCitiesInput {
  std::optional<std::string> nextToken;
  std::optional<int> pageSize;
};

struct ListCitiesOutput {
  std::optional<std::string> nextToken;
  std::vector<CitySummary> items;

  friend bool operator==(const ListCitiesOutput&, const ListCitiesOutput&) = default;
};

struct GetForecastInput {
  std::string cityId;
};

struct GetForecastOutput {
  std::optional<double> chanceOfRain;

  friend bool operator==(const GetForecastOutput&, const GetForecastOutput&) = default;
};

struct GetCurrentTimeOutput {
  opal::Timestamp time;

  friend bool operator==(const GetCurrentTimeOutput&, const GetCurrentTimeOutput&) = default;
};

// Document serde, one pair per structure — the shape of code the generator
// will emit. Deserializers reject missing @required members.
opal::Document SerializeGetCityOutput(const GetCityOutput& value);
opal::Outcome<GetCityOutput> DeserializeGetCityOutput(const opal::Document& doc);

opal::Document SerializeListCitiesOutput(const ListCitiesOutput& value);
opal::Outcome<ListCitiesOutput> DeserializeListCitiesOutput(const opal::Document& doc);

opal::Document SerializeGetForecastOutput(const GetForecastOutput& value);
opal::Outcome<GetForecastOutput> DeserializeGetForecastOutput(const opal::Document& doc);

opal::Document SerializeGetCurrentTimeOutput(const GetCurrentTimeOutput& value);
opal::Outcome<GetCurrentTimeOutput> DeserializeGetCurrentTimeOutput(const opal::Document& doc);

// Modeled error code shared by client and server ("NoSuchResource" per model).
inline constexpr char kNoSuchResourceCode[] = "NoSuchResource";

}  // namespace example::weather::handwritten

#endif  // SMITHY_EXAMPLES_WEATHER_HANDWRITTEN_WEATHER_TYPES_H_
