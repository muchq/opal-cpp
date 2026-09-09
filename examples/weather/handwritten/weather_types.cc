#include "examples/weather/handwritten/weather_types.h"

namespace example::weather::handwritten {
namespace {

using opal::Document;
using opal::DocumentList;
using opal::DocumentMap;
using opal::Error;
using opal::Outcome;

Error MissingMember(const char* name) {
  return Error::Serialization(std::string("weather: missing required member: ") + name);
}

Error WrongType(const char* name) {
  return Error::Serialization(std::string("weather: unexpected type for member: ") + name);
}

}  // namespace

Document SerializeGetCityOutput(const GetCityOutput& value) {
  DocumentMap coordinates;
  coordinates.emplace("latitude", Document(value.coordinates.latitude));
  coordinates.emplace("longitude", Document(value.coordinates.longitude));
  DocumentMap map;
  map.emplace("name", Document(value.name));
  map.emplace("coordinates", Document(std::move(coordinates)));
  return Document(std::move(map));
}

Outcome<GetCityOutput> DeserializeGetCityOutput(const Document& doc) {
  if (!doc.is_map()) return WrongType("GetCityOutput");
  GetCityOutput out;
  const Document* name = doc.Find("name");
  if (name == nullptr) return MissingMember("name");
  if (!name->is_string()) return WrongType("name");
  out.name = name->as_string();
  const Document* coordinates = doc.Find("coordinates");
  if (coordinates == nullptr) return MissingMember("coordinates");
  if (!coordinates->is_map()) return WrongType("coordinates");
  const Document* latitude = coordinates->Find("latitude");
  const Document* longitude = coordinates->Find("longitude");
  if (latitude == nullptr || longitude == nullptr) return MissingMember("coordinates.*");
  if ((!latitude->is_double() && !latitude->is_int()) ||
      (!longitude->is_double() && !longitude->is_int())) {
    return WrongType("coordinates.*");
  }
  out.coordinates.latitude = latitude->AsNumber();
  out.coordinates.longitude = longitude->AsNumber();
  return out;
}

Document SerializeListCitiesOutput(const ListCitiesOutput& value) {
  DocumentList items;
  items.reserve(value.items.size());
  for (const CitySummary& item : value.items) {
    DocumentMap entry;
    entry.emplace("cityId", Document(item.cityId));
    entry.emplace("name", Document(item.name));
    items.emplace_back(std::move(entry));
  }
  DocumentMap map;
  map.emplace("items", Document(std::move(items)));
  if (value.nextToken.has_value()) map.emplace("nextToken", Document(*value.nextToken));
  return Document(std::move(map));
}

Outcome<ListCitiesOutput> DeserializeListCitiesOutput(const Document& doc) {
  if (!doc.is_map()) return WrongType("ListCitiesOutput");
  ListCitiesOutput out;
  if (const Document* token = doc.Find("nextToken"); token != nullptr && !token->is_null()) {
    if (!token->is_string()) return WrongType("nextToken");
    out.nextToken = token->as_string();
  }
  const Document* items = doc.Find("items");
  if (items == nullptr) return MissingMember("items");
  if (!items->is_list()) return WrongType("items");
  for (const Document& entry : items->as_list()) {
    if (!entry.is_map()) return WrongType("items[]");
    const Document* city_id = entry.Find("cityId");
    const Document* name = entry.Find("name");
    if (city_id == nullptr || name == nullptr) return MissingMember("items[].*");
    if (!city_id->is_string() || !name->is_string()) return WrongType("items[].*");
    out.items.push_back(CitySummary{city_id->as_string(), name->as_string()});
  }
  return out;
}

Document SerializeGetForecastOutput(const GetForecastOutput& value) {
  DocumentMap map;
  if (value.chanceOfRain.has_value()) map.emplace("chanceOfRain", Document(*value.chanceOfRain));
  return Document(std::move(map));
}

Outcome<GetForecastOutput> DeserializeGetForecastOutput(const Document& doc) {
  if (!doc.is_map()) return WrongType("GetForecastOutput");
  GetForecastOutput out;
  if (const Document* chance = doc.Find("chanceOfRain"); chance != nullptr && !chance->is_null()) {
    if (!chance->is_double() && !chance->is_int()) return WrongType("chanceOfRain");
    out.chanceOfRain = chance->AsNumber();
  }
  return out;
}

Document SerializeGetCurrentTimeOutput(const GetCurrentTimeOutput& value) {
  DocumentMap map;
  // restJson1 serializes body timestamps as epoch-seconds by default.
  map.emplace("time", Document::FromTimestamp(value.time, opal::TimestampFormat::kEpochSeconds));
  return Document(std::move(map));
}

Outcome<GetCurrentTimeOutput> DeserializeGetCurrentTimeOutput(const Document& doc) {
  if (!doc.is_map()) return WrongType("GetCurrentTimeOutput");
  const Document* time = doc.Find("time");
  if (time == nullptr) return MissingMember("time");
  GetCurrentTimeOutput out;
  if (time->is_int() || time->is_double()) {
    out.time = opal::Timestamp::FromEpochSeconds(time->AsNumber());
  } else if (time->is_timestamp()) {
    out.time = time->as_timestamp().value;
  } else {
    return WrongType("time");
  }
  return out;
}

}  // namespace example::weather::handwritten
