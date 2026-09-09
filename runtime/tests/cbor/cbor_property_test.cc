// Property-based tests for the CBOR codec: encode/decode symmetry over random
// documents, and decoder robustness against corrupted input (the ASan/UBSan CI
// job gives the corruption loop real teeth).

#include <gtest/gtest.h>

#include <random>

#include "opal/cbor/cbor.h"
#include "tests/testing/random_document.h"

namespace opal::cbor {
namespace {

TEST(CborPropertyTest, RandomDocumentsRoundTrip) {
  opal::testing::RandomDocumentGenerator generator(/*seed=*/20260706);
  for (int i = 0; i < 500; ++i) {
    const Document original = generator.Next();
    const Blob encoded = Encode(original);
    const auto decoded = Decode(encoded);
    ASSERT_TRUE(decoded.ok()) << "iteration " << i << ": " << decoded.error().message();
    ASSERT_TRUE(*decoded == original) << "iteration " << i << " round trip mismatch";
  }
}

TEST(CborPropertyTest, EncodingIsDeterministic) {
  opal::testing::RandomDocumentGenerator generator(/*seed=*/777);
  for (int i = 0; i < 50; ++i) {
    const Document doc = generator.Next();
    ASSERT_TRUE(Encode(doc) == Encode(doc)) << "iteration " << i;
  }
}

// Decoding corrupted bytes must fail cleanly (an Error) or succeed with some
// value — never crash, hang, or trip a sanitizer.
TEST(CborPropertyTest, CorruptedInputNeverCrashes) {
  opal::testing::RandomDocumentGenerator generator(/*seed=*/424242);
  std::mt19937_64 rng(31337);
  for (int i = 0; i < 300; ++i) {
    Blob encoded = Encode(generator.Next());
    if (encoded.empty()) {
      continue;
    }
    auto& bytes = encoded.bytes();
    const int mutations = 1 + static_cast<int>(rng() % 4);
    for (int m = 0; m < mutations; ++m) {
      bytes[rng() % bytes.size()] = static_cast<std::uint8_t>(rng() % 256);
    }
    if (rng() % 4 == 0) {
      bytes.resize(bytes.size() - (rng() % bytes.size() + 1) % bytes.size());  // truncate
    }
    (void)Decode(encoded);  // outcome irrelevant; must not crash
  }
}

TEST(CborPropertyTest, RandomGarbageNeverCrashes) {
  std::mt19937_64 rng(987654321);
  for (int i = 0; i < 300; ++i) {
    std::vector<std::uint8_t> bytes(rng() % 64);
    for (auto& byte : bytes) {
      byte = static_cast<std::uint8_t>(rng() % 256);
    }
    (void)Decode(Blob(std::move(bytes)));
  }
}

}  // namespace
}  // namespace opal::cbor
