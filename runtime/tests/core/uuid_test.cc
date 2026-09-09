#include "smithy/core/uuid.h"

#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST(UuidTest, CanonicalForm) {
  const std::string uuid = opal::GenerateUuidV4();
  ASSERT_EQ(uuid.size(), 36u);
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      EXPECT_EQ(uuid[i], '-') << uuid;
    } else {
      EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(uuid[i]))) << uuid;
      EXPECT_FALSE(std::isupper(static_cast<unsigned char>(uuid[i]))) << uuid;
    }
  }
}

TEST(UuidTest, VersionAndVariantBits) {
  // RFC 4122: the version nibble is always 4, the variant nibble 10xx —
  // stable across every generated value, not just one sample.
  for (int i = 0; i < 256; ++i) {
    const std::string uuid = opal::GenerateUuidV4();
    EXPECT_EQ(uuid[14], '4') << uuid;
    EXPECT_TRUE(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b') << uuid;
  }
}

TEST(UuidTest, ValuesDoNotRepeat) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(seen.insert(opal::GenerateUuidV4()).second) << "duplicate uuid";
  }
}

TEST(UuidTest, ThreadLocalGeneratorsProduceDistinctStreams) {
  // The generator is thread_local and seeded per thread from random_device;
  // concurrent threads must not mirror each other's sequences.
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::vector<std::vector<std::string>> results(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&results, t] {
      results[t].reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) results[t].push_back(opal::GenerateUuidV4());
    });
  }
  for (auto& thread : threads) thread.join();
  std::set<std::string> all;
  for (const auto& batch : results) all.insert(batch.begin(), batch.end());
  EXPECT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace
