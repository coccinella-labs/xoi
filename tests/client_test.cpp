#include <xoi/agent.hpp>
#include <xoi/client.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class MockTransport final : public xoi::Transport {
public:
  explicit MockTransport(std::vector<xoi::HttpResponse> responses)
      : responses_(responses.begin(), responses.end()) {}

  xoi::HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec) override {
    urls.push_back(url);
    bodies.push_back(body);
    request_headers.push_back(headers);
    timeouts.push_back(timeout_sec);
    if (responses_.empty())
      return {0, {}, {}, "mock response queue exhausted"};
    auto response = std::move(responses_.front());
    responses_.pop_front();
    return response;
  }

  size_t calls() const {
    return urls.size();
  }

  std::vector<std::string> urls;
  std::vector<std::string> bodies;
  std::vector<std::vector<std::pair<std::string, std::string>>> request_headers;
  std::vector<int> timeouts;

private:
  std::deque<xoi::HttpResponse> responses_;
};

xoi::Config test_config(xoi::Provider provider = xoi::Provider::Anthropic) {
  xoi::Config config;
  config.provider = provider;
  config.api_key = "test-key";
  config.base_url = provider == xoi::Provider::Anthropic
                        ? "https://anthropic.test"
                        : "https://openai.test/v1/";
  config.initial_backoff_ms = 0;
  config.max_backoff_sec = 0;
  return config;
}

xoi::Request test_request() {
  xoi::Request request;
  request.model = "test-model";
  request.system = "Be concise.";
  request.messages = {{"user", {xoi::Text{"Hello"}}}};
  return request;
}

TEST(ClientTest, SerializesAnthropicMessagesAndToolSchema) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"text","text":"Hi"}],"stop_reason":"end_turn"})",
       {},
       {}}});
  auto *observed = mock.get();
  auto request = test_request();
  request.tools.push_back({"read_file",
                           "Read a file",
                           {{"type", "object"},
                            {"properties", {{"path", {{"type", "string"}}}}},
                            {"required", {"path"}}}});

  xoi::Client client(test_config(), std::move(mock));
  const auto result = client.send(request);

  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().content.size(), 1U);
  EXPECT_EQ(std::get<xoi::Text>(result.value().content[0]).text, "Hi");
  EXPECT_EQ(result.value().stop_reason, "end_turn");
  ASSERT_EQ(observed->calls(), 1U);
  EXPECT_EQ(observed->urls[0], "https://anthropic.test/v1/messages");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["system"], "Be concise.");
  EXPECT_EQ(sent["messages"][0]["content"][0]["text"], "Hello");
  EXPECT_EQ(sent["tools"][0]["input_schema"]["required"][0], "path");
  EXPECT_EQ(observed->timeouts[0], 60);

  const auto &headers = observed->request_headers[0];
  EXPECT_NE(std::find(headers.begin(), headers.end(),
                      std::make_pair(std::string("x-api-key"),
                                     std::string("test-key"))),
            headers.end());
}

TEST(ClientTest, ParsesAnthropicToolUse) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"read_file","input":{"path":"README.md"}}],"stop_reason":"tool_use"})",
       {},
       {}}});
  xoi::Client client(test_config(), std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().content.size(), 1U);
  const auto &tool = std::get<xoi::ToolUse>(result.value().content[0]);
  EXPECT_EQ(tool.id, "call-1");
  EXPECT_EQ(tool.name, "read_file");
  EXPECT_EQ(tool.input["path"], "README.md");
}

TEST(ClientTest, RetriesRateLimitAndHonorsRetryAfterHeader) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {429, R"({"error":{"message":"slow down"}})", {{"Retry-After", "0"}}, {}},
      {200, R"({"content":[{"type":"text","text":"ok"}]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  EXPECT_EQ(observed->calls(), 2U);
}

TEST(ClientTest, ReturnsNonRetryableClientErrors) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {400, R"({"error":{"message":"bad request"}})", {}, {}},
      {200, R"({"content":[]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 2;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Http);
  EXPECT_EQ(result.error().status, 400);
  EXPECT_EQ(result.error().message, "bad request");
  EXPECT_FALSE(result.error().retryable);
  EXPECT_EQ(observed->calls(), 1U);
}

TEST(ClientTest, RetriesServerErrors) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {503, R"({"error":"unavailable"})", {}, {}},
      {200, R"({"content":[{"type":"text","text":"recovered"}]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  EXPECT_EQ(observed->calls(), 2U);
}

TEST(ClientTest, ParsesOpenAICompatibleToolResponse) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"choices":[{"finish_reason":"tool_calls","message":{"content":null,"tool_calls":[{"id":"call-2","type":"function","function":{"name":"list_dir","arguments":"{\"path\":\".\"}"}}]}}]})",
       {},
       {}}});
  auto *observed = mock.get();
  auto config = test_config(xoi::Provider::OpenAICompatible);
  xoi::Client client(config, std::move(mock));
  auto request = test_request();
  request.tools.push_back(
      {"list_dir",
       "List a directory",
       {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}}});

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().content.size(), 1U);
  const auto &tool = std::get<xoi::ToolUse>(result.value().content[0]);
  EXPECT_EQ(tool.name, "list_dir");
  EXPECT_EQ(tool.input["path"], ".");
  EXPECT_EQ(observed->urls[0], "https://openai.test/v1/chat/completions");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["messages"][0]["role"], "system");
  EXPECT_EQ(sent["tools"][0]["function"]["name"], "list_dir");
  EXPECT_EQ(sent["tools"][0]["function"]["parameters"]["type"], "object");
  EXPECT_EQ(observed->request_headers[0].back().first, "Authorization");
  EXPECT_EQ(observed->request_headers[0].back().second, "Bearer test-key");
}

TEST(ClientTest, RejectsMissingCredentialsWithoutSending) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  auto config = test_config();
  config.api_key.clear();
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Configuration);
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(ClientTest, ReportsMalformedResponseAsProtocolError) {
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, "not-json", {}, {}}});
  xoi::Client client(test_config(), std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
}

TEST(ClientTest, RejectsResponseBodyAboveConfiguredLimit) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"content":[{"type":"text","text":"large"}]})", {}, {}}});
  auto config = test_config();
  config.max_response_bytes = 12;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::ResponseTooLarge);
  EXPECT_FALSE(result.error().retryable);
}

TEST(ClientTest, RejectsZeroResponseBodyLimit) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_response_bytes = 0;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Configuration);
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(ClientTest, ConvertsToolResultsForOpenAICompatibleRequests) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"choices":[{"message":{"content":"done"}}]})", {}, {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));
  auto request = test_request();
  request.messages = {
      {"assistant",
       {xoi::ToolUse{"call-1", "read_file", {{"path", "README.md"}}}}},
      {"user", {xoi::ToolResult{"call-1", "contents", false}}}};

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["messages"][1]["tool_calls"][0]["id"], "call-1");
  EXPECT_EQ(sent["messages"][2]["role"], "tool");
  EXPECT_EQ(sent["messages"][2]["content"], "contents");
}

TEST(AgentTest, ExecutesToolsAndSendsResultsBackToTheModel) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"read_file","input":{"path":"README.md"}}],"stop_reason":"tool_use"})",
       {},
       {}},
      {200,
       R"({"content":[{"type":"text","text":"The file says hello."}],"stop_reason":"end_turn"})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  EXPECT_TRUE(
      tools
          .add({"read_file",
                "Read a file",
                {{"type", "object"},
                 {"properties", {{"path", {{"type", "string"}}}}},
                 {"required", {"path"}}}},
               [](const nlohmann::json &input) -> xoi::Result<std::string> {
                 return "contents of " + input["path"].get<std::string>();
               })
          .empty());

  const auto result = xoi::run_agent(client, test_request(), tools);

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().text, "The file says hello.");
  EXPECT_EQ(result.value().steps, 2);
  EXPECT_EQ(result.value().tool_calls, 1);
  const auto follow_up = nlohmann::json::parse(observed->bodies[1]);
  EXPECT_EQ(follow_up["messages"][1]["content"][0]["type"], "tool_use");
  EXPECT_EQ(follow_up["messages"][2]["content"][0]["type"], "tool_result");
  EXPECT_EQ(follow_up["messages"][2]["content"][0]["content"],
            "contents of README.md");
}

TEST(AgentTest, ReturnsArgumentErrorsAsToolResultObservations) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"read_file","input":{"path":7}}],"stop_reason":"tool_use"})",
       {},
       {}},
      {200,
       R"({"content":[{"type":"text","text":"Use a string path."}]})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  EXPECT_TRUE(tools
                  .add({"read_file",
                        "Read a file",
                        {{"type", "object"},
                         {"properties", {{"path", {{"type", "string"}}}}},
                         {"required", {"path"}}}},
                       [](const nlohmann::json &) -> xoi::Result<std::string> {
                         ADD_FAILURE() << "invalid arguments must not execute";
                         return std::string{};
                       })
                  .empty());

  const auto result = xoi::run_agent(client, test_request(), tools);

  ASSERT_TRUE(result);
  const auto follow_up = nlohmann::json::parse(observed->bodies[1]);
  EXPECT_EQ(follow_up["messages"][2]["content"][0]["is_error"], true);
  EXPECT_EQ(follow_up["messages"][2]["content"][0]["content"],
            "argument 'path' must be string");
}

TEST(AgentTest, EnforcesStepLimit) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"noop","input":{}}]})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  int executions = 0;
  EXPECT_TRUE(tools
                  .add({"noop", "No operation", {{"type", "object"}}},
                       [&executions](const nlohmann::json &) {
                         ++executions;
                         return xoi::Result<std::string>(std::string{"done"});
                       })
                  .empty());

  const auto result =
      xoi::run_agent(client, test_request(), tools, xoi::AgentOptions{1, 2});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Limit);
  EXPECT_EQ(executions, 0);
  EXPECT_EQ(observed->calls(), 1U);
}

TEST(AgentTest, DoesNotExecuteIdenticalToolCallsAgain) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"noop","input":{}}]})",
       {},
       {}},
      {200,
       R"({"content":[{"type":"tool_use","id":"call-2","name":"noop","input":{}}]})",
       {},
       {}},
      {200, R"({"content":[{"type":"text","text":"done"}]})", {}, {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  int executions = 0;
  EXPECT_TRUE(tools
                  .add({"noop", "No operation", {{"type", "object"}}},
                       [&executions](const nlohmann::json &) {
                         ++executions;
                         return xoi::Result<std::string>(std::string{"first"});
                       })
                  .empty());

  const auto result = xoi::run_agent(client, test_request(), tools);

  ASSERT_TRUE(result);
  EXPECT_EQ(executions, 1);
  EXPECT_EQ(result.value().tool_calls, 2);
  const auto follow_up = nlohmann::json::parse(observed->bodies[2]);
  EXPECT_NE(
      follow_up["messages"][4]["content"][0]["content"].get<std::string>().find(
          "Earlier result:\nfirst"),
      std::string::npos);
  EXPECT_NE(
      follow_up["messages"][4]["content"][0]["content"].get<std::string>().find(
          "Hint: use this result"),
      std::string::npos);
}

} // namespace
