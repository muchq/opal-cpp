// Behavior tests for the checked-in generated code in examples/weather/generated
// (regenerate with: cd codegen && gradle generateFixtures).

#include <gtest/gtest.h>

#include <optional>

#include "example/weather/types.h"

namespace example::weather {
namespace {

TEST(WeatherGeneratedTypesTest, StructsSupportAggregateInitAndEquality) {
  const GetCityOutput a{.name = "Seattle", .coordinates = {.latitude = 47.6, .longitude = -122.3}};
  const GetCityOutput b{.name = "Seattle", .coordinates = {.latitude = 47.6, .longitude = -122.3}};
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a == GetCityOutput{});
}

TEST(WeatherGeneratedTypesTest, OptionalMembersDefaultToNullopt) {
  const ListCitiesInput input{};
  EXPECT_FALSE(input.nextToken.has_value());
  EXPECT_FALSE(input.pageSize.has_value());
  const GetForecastOutput forecast{};
  EXPECT_FALSE(forecast.chanceOfRain.has_value());
}

TEST(WeatherGeneratedTypesTest, RequiredMembersAreNotOptional) {
  GetCityInput input;
  input.cityId = "seattle";  // plain std::string, not optional
  EXPECT_EQ(input.cityId, "seattle");
  ListCitiesOutput output;
  output.items.push_back(CitySummary{.cityId = "a", .name = "A"});
  EXPECT_EQ(output.items.size(), 1u);
}

TEST(WeatherGeneratedTypesTest, ErrorShapesAreGenerated) {
  const NoSuchResource error{.resourceType = "City"};
  EXPECT_EQ(error.resourceType, "City");
}

TEST(WeatherGeneratedTypesTest, TimestampMembersUseRuntimeType) {
  GetCurrentTimeOutput output;
  output.time = opal::Timestamp::FromEpochMilliseconds(1000);
  EXPECT_EQ(output.time.epoch_milliseconds(), 1000);
}

}  // namespace
}  // namespace example::weather
