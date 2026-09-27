#include <catch2/catch_test_macros.hpp>

#include "proxy/base64.hpp"
#include "proxy/model_scan.hpp"

TEST_CASE("base64 roundtrip", "[proxy]") {
  using snowglobe::proxy::base64Decode;
  using snowglobe::proxy::base64Encode;
  CHECK(base64Encode("") == "");
  CHECK(base64Encode("f") == "Zg==");
  CHECK(base64Encode("fo") == "Zm8=");
  CHECK(base64Encode("foo") == "Zm9v");
  // Longer values: roundtrip (standard and url-safe spellings).
  const std::string url = "https://proxy.example:8443/base/path?q=1";
  std::string back;
  REQUIRE(base64Decode(base64Encode(url), back, false));
  CHECK(back == url);
  std::string urlsafe = base64Encode("http://localhost:9919");
  for (char& c : urlsafe) {
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    }
  }
  while (!urlsafe.empty() && urlsafe.back() == '=') {
    urlsafe.pop_back();
  }
  REQUIRE(base64Decode(urlsafe, back, true));
  CHECK(back == "http://localhost:9919");
  // garbage rejected
  std::string out;
  CHECK(!base64Decode("!!!not-base64!!!", out, false));
  CHECK(!base64Decode("!!!not-base64!!!", out, true));
}

TEST_CASE("isUtf8", "[proxy]") {
  using snowglobe::proxy::isUtf8;
  CHECK(isUtf8(""));
  CHECK(isUtf8("{\"model\":\"x\"}"));
  CHECK(isUtf8("héllo"));
  CHECK(!isUtf8(std::string("abc\xff") + "def"));
  CHECK(!isUtf8(std::string("abc\x80")));
}

TEST_CASE("scanJsonTop model/stream", "[proxy]") {
  using snowglobe::proxy::scanJsonTop;
  {
    const auto r = scanJsonTop("{\"model\":\"gpt-4o\",\"stream\":true}");
    CHECK(r.isJson);
    CHECK(r.hasModel);
    CHECK(r.model == "gpt-4o");
    CHECK(r.hasStream);
    CHECK(r.stream);
  }
  {
    const auto r = scanJsonTop(" { \"stream\" : false , \"messages\": [] } ");
    CHECK(r.isJson);
    CHECK(!r.hasModel);
    CHECK(r.hasStream);
    CHECK(!r.stream);
  }
  {
    // Nested "model" (tool parameter name) must not match.
    const auto r = scanJsonTop("{\"tools\":[{\"function\":{\"name\":\"f\",\"parameters\":{"
                               "\"model\":{\"type\":\"string\"}}}}]}");
    CHECK(r.isJson);
    CHECK(!r.hasModel);
  }
  {
    // Non-string model is present-but-unusable.
    const auto r = scanJsonTop("{\"model\":null}");
    CHECK(r.isJson);
    CHECK(!r.hasModel);
  }
  {
    // Not objects: null fields.
    CHECK(!scanJsonTop("[1,2]").isJson);
    CHECK(!scanJsonTop("event: message").isJson);
    CHECK(!scanJsonTop("").isJson);
  }
  {
    // Truncated bodies still yield what was seen.
    const auto r = scanJsonTop("{\"model\":\"gpt-4");
    CHECK(r.isJson);
    CHECK(!r.hasModel); // unterminated string: no usable value
  }
}
