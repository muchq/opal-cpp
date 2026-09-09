// Phase 3 bridge test: the GENERATED restJson1 weather client talks to the
// hand-written weather server from Phase 1 — first half of the
// generated-client <-> generated-server harness (PLAN Phase 5).

#include <gtest/gtest.h>

#include <memory>

#include "example/weather/client.h"
#include "examples/weather/handwritten/weather_server.h"
#include "opal/http/loopback.h"
#include "opal/http/socket_transport.h"

namespace example::weather {
namespace {

namespace hw = example::weather::handwritten;

class ReferenceHandler final : public hw::WeatherHandler {
 public:
  opal::Outcome<hw::GetCityOutput> GetCity(const hw::GetCityInput& input) override {
    if (input.cityId == "seattle") {
      return hw::GetCityOutput{"Seattle", hw::CityCoordinates{47.6062, -122.3321}};
    }
    if (input.cityId == "rain city") {
      return hw::GetCityOutput{"Rain City", hw::CityCoordinates{45.0, -120.0}};
    }
    return opal::Error::Modeled(hw::kNoSuchResourceCode, "no city: " + input.cityId);
  }

  opal::Outcome<hw::ListCitiesOutput> ListCities(const hw::ListCitiesInput& input) override {
    hw::ListCitiesOutput out;
    if (!input.nextToken.has_value()) {
      out.items.push_back(hw::CitySummary{"seattle", "Seattle"});
      if (input.pageSize.value_or(2) < 2) {
        out.nextToken = "page-2";
        return out;
      }
    }
    out.items.push_back(hw::CitySummary{"rain city", "Rain City"});
    return out;
  }

  opal::Outcome<hw::GetForecastOutput> GetForecast(const hw::GetForecastInput& input) override {
    if (input.cityId != "seattle") {
      return opal::Error::Modeled(hw::kNoSuchResourceCode, "no city: " + input.cityId);
    }
    return hw::GetForecastOutput{0.75};
  }

  opal::Outcome<hw::GetCurrentTimeOutput> GetCurrentTime() override {
    return hw::GetCurrentTimeOutput{opal::Timestamp::FromEpochMilliseconds(1398796238500)};
  }
};

enum class Transport { kLoopback, kSocket };

class GeneratedClientEndToEndTest : public testing::TestWithParam<Transport> {
 protected:
  void SetUp() override {
    service_ = std::make_unique<hw::WeatherService>(std::make_shared<ReferenceHandler>());
    opal::ClientConfig config;
    if (GetParam() == Transport::kLoopback) {
      auto loopback = std::make_shared<opal::http::Loopback>();
      ASSERT_TRUE(loopback->Start(service_->Handler()).ok());
      config.http_client = loopback;
    } else {
      socket_server_ = std::make_unique<opal::http::SocketHttpServer>();
      ASSERT_TRUE(socket_server_->Start(service_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(socket_server_->port());
    }
    auto client = WeatherClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<WeatherClient>(std::move(*client));
  }

  void TearDown() override {
    if (socket_server_ != nullptr) socket_server_->Stop();
  }

  std::unique_ptr<hw::WeatherService> service_;
  std::unique_ptr<opal::http::SocketHttpServer> socket_server_;
  std::unique_ptr<WeatherClient> client_;
};

TEST_P(GeneratedClientEndToEndTest, GetCityRoundTrips) {
  const auto city = client_->GetCity(GetCityInput{.cityId = "seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Seattle");
  // The model declares Float (32-bit): precision is float, not double.
  EXPECT_FLOAT_EQ(city->coordinates.latitude, 47.6062F);
  EXPECT_FLOAT_EQ(city->coordinates.longitude, -122.3321F);
}

TEST_P(GeneratedClientEndToEndTest, PercentEncodedLabels) {
  const auto city = client_->GetCity(GetCityInput{.cityId = "rain city"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Rain City");
}

TEST_P(GeneratedClientEndToEndTest, ModeledErrorsSurfaceTyped) {
  const auto city = client_->GetCity(GetCityInput{.cityId = "atlantis"});
  ASSERT_FALSE(city.ok());
  EXPECT_EQ(city.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(city.error().code(), "NoSuchResource");
  EXPECT_EQ(city.error().message(), "no city: atlantis");
}

TEST_P(GeneratedClientEndToEndTest, QueryBindingsPaginate) {
  const auto first = client_->ListCities(ListCitiesInput{.nextToken = std::nullopt, .pageSize = 1});
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_EQ(first->items.size(), 1u);
  ASSERT_TRUE(first->nextToken.has_value());

  const auto second =
      client_->ListCities(ListCitiesInput{.nextToken = first->nextToken, .pageSize = 1});
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(second->items.size(), 1u);
  EXPECT_EQ(second->items[0].cityId, "rain city");
  EXPECT_FALSE(second->nextToken.has_value());
}

TEST_P(GeneratedClientEndToEndTest, OptionalOutputMembers) {
  const auto forecast = client_->GetForecast(GetForecastInput{.cityId = "seattle"});
  ASSERT_TRUE(forecast.ok()) << forecast.error().message();
  EXPECT_FLOAT_EQ(forecast->chanceOfRain.value(), 0.75F);
}

TEST_P(GeneratedClientEndToEndTest, TimestampsRoundTrip) {
  const auto time = client_->GetCurrentTime();
  ASSERT_TRUE(time.ok()) << time.error().message();
  EXPECT_EQ(time->time.epoch_milliseconds(), 1398796238500);
}

INSTANTIATE_TEST_SUITE_P(Transports, GeneratedClientEndToEndTest,
                         testing::Values(Transport::kLoopback, Transport::kSocket),
                         [](const testing::TestParamInfo<Transport>& info) {
                           return info.param == Transport::kLoopback ? "Loopback" : "Sockets";
                         });

}  // namespace
}  // namespace example::weather
