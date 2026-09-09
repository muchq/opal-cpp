#ifndef SMITHY_JSON_JSON_H_
#define SMITHY_JSON_JSON_H_

#include <string>
#include <string_view>

#include "smithy/core/document.h"
#include "smithy/core/outcome.h"

namespace opal::json {

// Renders a Document as compact JSON text with Smithy JSON conventions:
// blob nodes become base64 strings; timestamp nodes follow their stored
// format (epoch-seconds as a number, date-time/http-date as strings). Map
// keys are emitted in sorted order, so output is deterministic.
std::string Encode(const Document& doc);

// Parses JSON text into a Document. The result contains only JSON-native
// nodes (null/bool/int/double/string/list/map): whether a string is a blob
// or a timestamp is shape knowledge that belongs to the calling serde code,
// which re-types members via Base64Decode / Timestamp::Parse.
Outcome<Document> Decode(std::string_view text);

}  // namespace opal::json

#endif  // SMITHY_JSON_JSON_H_
