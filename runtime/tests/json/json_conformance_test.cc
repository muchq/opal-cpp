// Runs the vendored JSONTestSuite parsing corpus (nst/JSONTestSuite, see
// tests/json/jsontestsuite/PROVENANCE.md) through opal::json::Decode. The
// suite is the canonical bank for "does the parser agree with RFC 8259 and
// never crash on hostile input" — the class of bug that produced the
// nesting-depth stack overflow this test was added with.
//
// Invariant for every file: Decode returns, never crashes or hangs. On top of
// that, y_ files must be accepted and n_ files rejected (with a documented
// allowlist); i_ files are implementation-defined, so no-crash is the only
// requirement.

#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "opal/json/json.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace opal::json {

// Defined in the generated json_conformance_index.cc (a genrule lists the
// vendored corpus filenames): runfiles directory enumeration is not portable,
// so the filenames are baked in at build time from the checked-in files.
std::vector<std::string> CorpusFiles();

namespace {

using bazel::tools::cpp::runfiles::Runfiles;

// n_ cases the nlohmann backend accepts. Documented, not fixed — see
// PROVENANCE.md. "123\0": a number then a NUL byte, tolerated as trailing
// whitespace.
const std::set<std::string>& AcceptedNegatives() {
  static const std::set<std::string> kAllow = {"n_multidigit_number_then_00.json"};
  return kAllow;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

class JsonConformanceTest : public testing::TestWithParam<std::string> {};

TEST_P(JsonConformanceTest, MatchesRfc8259AndNeverCrashes) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  ASSERT_NE(runfiles, nullptr) << error;

  const std::string name = GetParam();
  const std::string path =
      runfiles->Rlocation("_main/runtime/tests/json/jsontestsuite/test_parsing/" + name);
  const std::string content = ReadFile(path);

  // The load-bearing assertion: this call returns rather than crashing or
  // hanging, whatever the verdict.
  const auto decoded = Decode(content);

  switch (name[0]) {
    case 'y':
      EXPECT_TRUE(decoded.ok()) << "valid JSON rejected: " << decoded.error().message();
      break;
    case 'n':
      if (AcceptedNegatives().count(name) == 0) {
        EXPECT_FALSE(decoded.ok()) << "invalid JSON accepted";
      }
      break;
    default:
      break;  // i_: implementation-defined, no-crash already asserted.
  }
}

INSTANTIATE_TEST_SUITE_P(JSONTestSuite, JsonConformanceTest, testing::ValuesIn(CorpusFiles()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           // Distinct filenames can sanitize to the same
                           // identifier (1.0e- vs 1.0e+), so prefix the index.
                           std::string id = std::to_string(info.index) + "_" + info.param;
                           for (char& c : id) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return id;
                         });

}  // namespace
}  // namespace opal::json
