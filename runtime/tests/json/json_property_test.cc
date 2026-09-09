// Property-based JSON round trips over the JSON-native Document subset
// (blob/timestamp nodes re-type through JSON text, so node-identity round
// trips only hold for the native kinds).

#include <gtest/gtest.h>

#include "smithy/json/json.h"
#include "tests/testing/random_document.h"

namespace opal::json {
namespace {

TEST(JsonPropertyTest, RandomNativeDocumentsRoundTrip) {
  opal::testing::DocumentGeneratorOptions options;
  options.with_blobs = false;
  options.with_timestamps = false;
  opal::testing::RandomDocumentGenerator generator(/*seed=*/20260706, options);
  for (int i = 0; i < 500; ++i) {
    const Document original = generator.Next();
    const std::string text = Encode(original);
    const auto decoded = Decode(text);
    ASSERT_TRUE(decoded.ok()) << "iteration " << i << ": " << decoded.error().message()
                              << "\ntext: " << text;
    ASSERT_TRUE(*decoded == original) << "iteration " << i << " mismatch\ntext: " << text;
  }
}

TEST(JsonPropertyTest, EncodingIsDeterministic) {
  opal::testing::DocumentGeneratorOptions options;
  options.with_blobs = false;
  options.with_timestamps = false;
  opal::testing::RandomDocumentGenerator generator(/*seed=*/99, options);
  for (int i = 0; i < 50; ++i) {
    const Document doc = generator.Next();
    ASSERT_EQ(Encode(doc), Encode(doc)) << "iteration " << i;
  }
}

}  // namespace
}  // namespace opal::json
