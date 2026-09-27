#include <catch2/catch_test_macros.hpp>

#include "redact/redact.hpp"

namespace {
const std::vector<std::pair<std::string, std::string>> kNoEnv;
std::string r(const std::string& s) {
  return snowglobe::redact::redactText(s, kNoEnv);
}
} // namespace

TEST_CASE("redacted headers", "[redact]") {
  using snowglobe::redact::redactHeaderValue;
  CHECK(redactHeaderValue("Authorization", "Bearer abc") == "REDACTED");
  CHECK(redactHeaderValue("authorization", "Bearer abc") == "REDACTED");
  CHECK(redactHeaderValue("X-API-Key", "k") == "REDACTED");
  CHECK(redactHeaderValue("x-api-key", "k") == "REDACTED");
  CHECK(redactHeaderValue("Api-Key", "k") == "REDACTED");
  CHECK(redactHeaderValue("Cookie", "s=1") == "REDACTED");
  CHECK(redactHeaderValue("Set-Cookie", "s=1") == "REDACTED");
  CHECK(redactHeaderValue("Proxy-Authorization", "x") == "REDACTED");
  CHECK(redactHeaderValue("X-Goog-Api-Key", "k") == "REDACTED");
  CHECK(redactHeaderValue("Content-Type", "application/json") == "application/json");
  CHECK(redactHeaderValue("Content-Length", "12") == "12");
  CHECK(redactHeaderValue("Host", "api.openai.com") == "api.openai.com");
  CHECK(redactHeaderValue("X-Request-Id", "abc") == "abc");
}

TEST_CASE("URL userinfo", "[redact]") {
  CHECK(r("https://user:s3cret@host/x") == "https://user:REDACTED@host/x");
  CHECK(r("https://user@host/x") == "https://REDACTED@host/x");
  CHECK(r("postgres://bob:pw1234@db:5432/app") == "postgres://bob:REDACTED@db:5432/app");
  // No scheme: emails and bare host:port forms survive.
  CHECK(r("mail bob@example.com today") == "mail bob@example.com today");
  CHECK(r("connect host:8080 now") == "connect host:8080 now");
}

TEST_CASE("bearer tokens", "[redact]") {
  CHECK(r("Authorization: Bearer sk-test-1234567890") == "Authorization: Bearer REDACTED");
  CHECK(r("authorization: bearer ABCDEFGH") == "authorization: Bearer REDACTED");
  CHECK(r("Proxy auth Basic dXNlcjpwYXNzd29yZA==") == "Proxy auth Basic REDACTED");
  // Short tokens look like prose: untouched.
  CHECK(r("the bearer of bad news") == "the bearer of bad news");
  CHECK(r("Bearer") == "Bearer");
}

TEST_CASE("key shapes", "[redact]") {
  CHECK(r("key=sk-ant-api03-abcdefghij1234567890") == "key=REDACTED");
  CHECK(r("token sk-abcdefghijklmnopqrstuvwxyz1234567890 end") == "token REDACTED end");
  // Short sk- looks like a word fragment: untouched.
  CHECK(r("the sk-foo flag") == "the sk-foo flag");
  CHECK(r("ghp_abcdefghijklmnopqrstuvwxyz1234567890") == "REDACTED");
  CHECK(r("gho_abcdefghijklmnopqrstuvwxyz1234567890") == "REDACTED");
  CHECK(r("github_pat_abcdefghijklmnopqrstuvwxyz1234567890") == "REDACTED");
  CHECK(r("key AKIAIOSFODNN7EXAMPLE end") == "key REDACTED end");
  // 17 alnum after AKIA: not the AWS shape, untouched.
  CHECK(r("AKIAIOSFODNN7EXAMPLEX") == "AKIAIOSFODNN7EXAMPLEX");
  CHECK(r("xoxb-12345-67890-abcdefghij") == "REDACTED");
  CHECK(r("xoxz-12345-67890-abcdefghij") == "xoxz-12345-67890-abcdefghij");
}

TEST_CASE("query params", "[redact]") {
  CHECK(r("https://h/p?key=SECRET123&x=1") == "https://h/p?key=REDACTED&x=1");
  CHECK(r("https://h/p?x=1&token=SECRET123") == "https://h/p?x=1&token=REDACTED");
  CHECK(r("https://h/p?PASSWORD=SECRET123") == "https://h/p?PASSWORD=REDACTED");
  // Empty values and unrelated params survive.
  CHECK(r("https://h/p?key=&x=1") == "https://h/p?key=&x=1");
  CHECK(r("https://h/p?monkey=banana") == "https://h/p?monkey=banana");
}

TEST_CASE("env values", "[redact]") {
  using snowglobe::redact::redactText;
  using Pair = std::pair<std::string, std::string>;
  // sensitiveEnv filters by name; redactText only sees values.
  const std::vector<Pair> env = {{"MY_API_KEY", "supersecretvalue123"}};
  CHECK(redactText("curl -H X: supersecretvalue123", env) == "curl -H X: REDACTED");
  // Longest value wins on overlap.
  const std::vector<Pair> overlap = {{"A_TOKEN", "secret"}, {"B_TOKEN", "secretvalue"}};
  CHECK(redactText("use secretvalue here", overlap) == "use REDACTED here");
}

TEST_CASE("sensitiveEnv filtering", "[redact]") {
  using snowglobe::redact::sensitiveEnv;
  char k1[] = "MY_API_KEY=supersecretvalue123";
  char k2[] = "SHORT_KEY=x";
  char k3[] = "PLAIN_NAME=supersecretvalue123";
  char k4[] = "COMPASS=supersecretvalue123";
  char* envp[] = {k1, k2, k3, k4, nullptr};
  const auto out = sensitiveEnv(envp);
  REQUIRE(out.size() == 1);
  CHECK(out[0].first == "MY_API_KEY");
  CHECK(out[0].second == "supersecretvalue123");
}

TEST_CASE("non-matches survive", "[redact]") {
  // UUIDs and git SHAs must never be redacted.
  CHECK(r("id 123e4567-e89b-42d3-a456-426614174000 done") ==
        "id 123e4567-e89b-42d3-a456-426614174000 done");
  CHECK(r("commit da39a3ee5e6b4b0d3255bfef95601890afd80709 ok") ==
        "commit da39a3ee5e6b4b0d3255bfef95601890afd80709 ok");
  CHECK(r("run the test suite now") == "run the test suite now");
  CHECK(r("password is required") == "password is required");
  CHECK(r("task-schedule --daily") == "task-schedule --daily");
}
