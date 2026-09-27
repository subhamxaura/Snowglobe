#include <catch2/catch_test_macros.hpp>

#include "util/string_util.hpp"

TEST_CASE("jsonEscape quotes and controls", "[json]") {
  using snowglobe::util::jsonEscape;
  CHECK(jsonEscape("") == "\"\"");
  CHECK(jsonEscape("abc") == "\"abc\"");
  CHECK(jsonEscape("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(jsonEscape("a\nb\tc\rb") == "\"a\\nb\\tc\\rb\"");
  CHECK(jsonEscape(std::string("a\x01"
                               "b")) == "\"a\\u0001b\"");
  // UTF-8 passes through untouched.
  CHECK(jsonEscape("héllo") == "\"héllo\"");
}
