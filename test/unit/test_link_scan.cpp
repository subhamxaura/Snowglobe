#include <catch2/catch_test_macros.hpp>

#include "link/event_scan.hpp"
#include "link/tools.hpp"

TEST_CASE("findField reads top-level fields only", "[link]") {
  using snowglobe::link::findField;
  using snowglobe::link::readField;
  const std::string line = "{\"ev\":\"proc.exec\",\"pid\":123,\"thread\":true,\"model\":null,"
                           "\"nested\":{\"ev\":\"decoy\"},\"argv\":[\"sh\",\"-c\",\"echo hi\"]}";
  size_t v = 0;
  REQUIRE(findField(line, 0, "ev", v));
  CHECK(readField(line, v).str == "proc.exec");
  REQUIRE(findField(line, 0, "pid", v));
  const auto pid = readField(line, v);
  CHECK(pid.isNumber);
  CHECK(pid.num == 123);
  REQUIRE(findField(line, 0, "thread", v));
  CHECK(readField(line, v).boolean);
  // null is found but typeless (model:null reads as no model)
  REQUIRE(findField(line, 0, "model", v));
  const auto model = readField(line, v);
  CHECK(model.found);
  CHECK(!model.isString);
  // absent key
  CHECK(!findField(line, 0, "nope", v));
  // nested decoy never shadows the top-level key
  REQUIRE(findField(line, 0, "ev", v));
  CHECK(readField(line, v).str == "proc.exec");
  // array flag (elements read separately)
  REQUIRE(findField(line, 0, "argv", v));
  CHECK(readField(line, v).isArray);
}

TEST_CASE("getStrArray accepts string arrays, rejects the rest", "[link]") {
  using snowglobe::link::findField;
  using snowglobe::link::getStrArray;
  const std::string line =
      "{\"argv\":[\"sh\",\"-c\",\"echo \\\"q\\\"\"],\"empty\":[],\"mixed\":[\"a\",1],"
      "\"trunc\":[\"a\",";
  size_t v = 0;
  std::vector<std::string> out;
  REQUIRE(findField(line, 0, "argv", v));
  REQUIRE(getStrArray(line, v, out));
  CHECK(out == std::vector<std::string>{"sh", "-c", "echo \"q\""});
  REQUIRE(findField(line, 0, "empty", v));
  REQUIRE(getStrArray(line, v, out));
  CHECK(out.empty());
  REQUIRE(findField(line, 0, "mixed", v));
  CHECK(!getStrArray(line, v, out)); // non-string element fails all
  REQUIRE(findField(line, 0, "trunc", v));
  CHECK(!getStrArray(line, v, out)); // unterminated fails
}

TEST_CASE("extractTools reads OpenAI tool_calls", "[link]") {
  using snowglobe::link::extractTools;
  const std::string body =
      "{\"choices\":[{\"message\":{\"tool_calls\":["
      "{\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
      "\"name\":\"run_command\",\"arguments\":\"{\\\"command\\\": \\\"echo hi\\\"}\"}}]}}]}";
  const auto tools = extractTools(body, "openai");
  REQUIRE(tools.size() == 1);
  CHECK(tools[0].id == "call_1");
  CHECK(tools[0].name == "run_command");
  CHECK(tools[0].command == "echo hi");
  // no tool_calls → no tools, never throws
  CHECK(extractTools("{\"choices\":[{\"message\":{\"content\":\"done\"}}]}", "openai").empty());
}

TEST_CASE("extractTools reads Anthropic tool_use, depth-1 command only", "[link]") {
  using snowglobe::link::extractTools;
  const std::string body =
      "{\"content\":[{\"type\":\"text\",\"text\":\"x\"},"
      "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Bash\","
      "\"input\":{\"command\":\"ls -la\",\"nested\":{\"command\":\"decoy\"}}}]}";
  const auto tools = extractTools(body, "anthropic");
  REQUIRE(tools.size() == 1);
  CHECK(tools[0].id == "toolu_1");
  CHECK(tools[0].name == "Bash");
  CHECK(tools[0].command == "ls -la"); // nested decoy ignored
  // error envelopes and binary are not tool calls
  CHECK(extractTools("{\"type\":\"error\",\"error\":{\"type\":\"auth\"}}", "anthropic").empty());
  CHECK(extractTools("not json{{", "anthropic").empty());
  CHECK(extractTools("data: {\"type\":\"message_start\"}\n\n", "anthropic").empty());
}

TEST_CASE("extractTools unknown provider tries both shapes", "[link]") {
  using snowglobe::link::extractTools;
  const std::string body = "{\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"Bash\","
                           "\"input\":{\"command\":\"pwd\"}}]}";
  const auto tools = extractTools(body, "custom");
  REQUIRE(tools.size() == 1);
  CHECK(tools[0].command == "pwd");
}
