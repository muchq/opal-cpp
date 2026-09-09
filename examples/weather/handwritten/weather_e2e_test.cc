// Phase 1 exit criterion (PLAN §Phase 1): a hand-written weather client and
// server, both built on the runtime, complete real requests over an in-memory
// loopback AND real TCP sockets — with the same test body.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "examples/weather/handwritten/weather_client.h"
#include "examples/weather/handwritten/weather_server.h"
#include "smithy/http/loopback.h"
#include "smithy/http/socket_transport.h"
#ifdef SMITHY_E2E_HAVE_BEAST
#include "smithy/http/beast_transport.h"
#endif

namespace example::weather::handwritten {
namespace {

// Reference handler: two cities, deterministic time.
class ReferenceHandler final : public WeatherHandler {
 public:
  opal::Outcome<GetCityOutput> GetCity(const GetCityInput& input) override {
    if (input.cityId == "seattle") {
      return GetCityOutput{"Seattle", CityCoordinates{47.6062, -122.3321}};
    }
    if (input.cityId == "rain city") {  // exercises percent-encoded labels
      return GetCityOutput{"Rain City", CityCoordinates{45.0, -120.0}};
    }
    return opal::Error::Modeled(kNoSuchResourceCode, "no city: " + input.cityId);
  }

  opal::Outcome<ListCitiesOutput> ListCities(const ListCitiesInput& input) override {
    ListCitiesOutput out;
    if (!input.nextToken.has_value()) {
      out.items.push_back(CitySummary{"seattle", "Seattle"});
      if (input.pageSize.value_or(2) < 2) {
        out.nextToken = "page-2";
        return out;
      }
    }
    out.items.push_back(CitySummary{"rain city", "Rain City"});
    return out;
  }

  opal::Outcome<GetForecastOutput> GetForecast(const GetForecastInput& input) override {
    if (input.cityId != "seattle") {
      return opal::Error::Modeled(kNoSuchResourceCode, "no city: " + input.cityId);
    }
    return GetForecastOutput{0.75};
  }

  opal::Outcome<GetCurrentTimeOutput> GetCurrentTime() override {
    return GetCurrentTimeOutput{opal::Timestamp::FromEpochMilliseconds(1398796238500)};
  }
};

enum class Transport { kLoopback, kSocket, kBeast };

class WeatherEndToEndTest : public testing::TestWithParam<Transport> {
 protected:
  void SetUp() override {
    service_ = std::make_unique<WeatherService>(std::make_shared<ReferenceHandler>());
    opal::ClientConfig config;
    if (GetParam() == Transport::kLoopback) {
      auto loopback = std::make_shared<opal::http::Loopback>();
      ASSERT_TRUE(loopback->Start(service_->Handler()).ok());
      transport_holder_ = loopback;
      config.http_client = loopback;
    } else if (GetParam() == Transport::kSocket) {
      socket_server_ = std::make_unique<opal::http::SocketHttpServer>();
      ASSERT_TRUE(socket_server_->Start(service_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(socket_server_->port());
    } else {
#ifdef SMITHY_E2E_HAVE_BEAST
      beast_server_ = std::make_unique<opal::http::BeastServerTransport>();
      ASSERT_TRUE(beast_server_->Start(service_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(beast_server_->port());
#else
      GTEST_FAIL() << "Beast transport not compiled into this test binary";
#endif
    }
    auto client = WeatherClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<WeatherClient>(std::move(*client));
  }

  void TearDown() override {
    if (socket_server_ != nullptr) socket_server_->Stop();
#ifdef SMITHY_E2E_HAVE_BEAST
    if (beast_server_ != nullptr) beast_server_->Stop();
#endif
  }

  std::unique_ptr<WeatherService> service_;
  std::shared_ptr<opal::http::HttpClient> transport_holder_;
  std::unique_ptr<opal::http::SocketHttpServer> socket_server_;
#ifdef SMITHY_E2E_HAVE_BEAST
  std::unique_ptr<opal::http::BeastServerTransport> beast_server_;
#endif
  std::unique_ptr<WeatherClient> client_;
};

TEST_P(WeatherEndToEndTest, GetCityRoundTrips) {
  const auto city = client_->GetCity(GetCityInput{"seattle"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(*city, (GetCityOutput{"Seattle", CityCoordinates{47.6062, -122.3321}}));
}

TEST_P(WeatherEndToEndTest, LabelsWithSpacesAreEncodedCorrectly) {
  const auto city = client_->GetCity(GetCityInput{"rain city"});
  ASSERT_TRUE(city.ok()) << city.error().message();
  EXPECT_EQ(city->name, "Rain City");
}

TEST_P(WeatherEndToEndTest, ModeledErrorsSurfaceWithCodeAndStatus) {
  const auto city = client_->GetCity(GetCityInput{"atlantis"});
  ASSERT_FALSE(city.ok());
  EXPECT_EQ(city.error().kind(), opal::ErrorKind::kModeled);
  EXPECT_EQ(city.error().code(), kNoSuchResourceCode);
  EXPECT_EQ(city.error().message(), "no city: atlantis");
}

TEST_P(WeatherEndToEndTest, ListCitiesPaginates) {
  const auto first = client_->ListCities(ListCitiesInput{std::nullopt, 1});
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_EQ(first->items.size(), 1u);
  ASSERT_TRUE(first->nextToken.has_value());

  const auto second = client_->ListCities(ListCitiesInput{first->nextToken, 1});
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(second->items.size(), 1u);
  EXPECT_EQ(second->items[0].cityId, "rain city");
  EXPECT_FALSE(second->nextToken.has_value());
}

TEST_P(WeatherEndToEndTest, GetForecastReturnsOptionalMember) {
  const auto forecast = client_->GetForecast(GetForecastInput{"seattle"});
  ASSERT_TRUE(forecast.ok()) << forecast.error().message();
  EXPECT_DOUBLE_EQ(forecast->chanceOfRain.value(), 0.75);
}

TEST_P(WeatherEndToEndTest, GetCurrentTimeRoundTripsTimestamp) {
  const auto time = client_->GetCurrentTime();
  ASSERT_TRUE(time.ok()) << time.error().message();
  EXPECT_EQ(time->time.epoch_milliseconds(), 1398796238500);
}

#ifdef SMITHY_E2E_HAVE_BEAST
INSTANTIATE_TEST_SUITE_P(Transports, WeatherEndToEndTest,
                         testing::Values(Transport::kLoopback, Transport::kSocket,
                                         Transport::kBeast),
                         [](const testing::TestParamInfo<Transport>& info) {
                           switch (info.param) {
                             case Transport::kLoopback:
                               return "Loopback";
                             case Transport::kSocket:
                               return "Sockets";
                             default:
                               return "Beast";
                           }
                         });
#else
INSTANTIATE_TEST_SUITE_P(Transports, WeatherEndToEndTest,
                         testing::Values(Transport::kLoopback, Transport::kSocket),
                         [](const testing::TestParamInfo<Transport>& info) {
                           return info.param == Transport::kLoopback ? "Loopback" : "Sockets";
                         });
#endif

}  // namespace
}  // namespace example::weather::handwritten
