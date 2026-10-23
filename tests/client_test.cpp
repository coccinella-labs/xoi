#include <xoi/agent.hpp>
#include <xoi/client.hpp>

#include "../src/curl_body_callback.hpp"
#include "../src/curl_option.hpp"
#include "../src/environment.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
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
       int timeout_sec, std::size_t max_response_bytes) override {
    urls.push_back(url);
    bodies.push_back(body);
    request_headers.push_back(headers);
    timeouts.push_back(timeout_sec);
    response_limits.push_back(max_response_bytes);
    if (responses_.empty())
      return {0, {}, {}, "mock response queue exhausted"};
    auto response = std::move(responses_.front());
    responses_.pop_front();
    return response;
  }

  xoi::HttpResponse
  post_stream(const std::string &url, const std::string &body,
              const std::vector<std::pair<std::string, std::string>> &headers,
              int timeout_sec, std::size_t max_response_bytes,
              const xoi::StreamChunkHandler &on_chunk) override {
    auto response = post(url, body, headers, timeout_sec, max_response_bytes);
    if (response.transport_error.empty() && response.status >= 200 &&
        response.status < 300 && !response.response_too_large &&
        !response.response_callback_failed) {
      try {
        constexpr std::size_t chunk_size = 7;
        for (std::size_t offset = 0; offset < response.body.size();
             offset += chunk_size) {
          const auto size =
              (std::min)(chunk_size, response.body.size() - offset);
          on_chunk(std::string_view(response.body.data() + offset, size));
        }
        response.response_bytes_received = response.body.size();
      } catch (const std::exception &error) {
        response.response_callback_failed = true;
        response.transport_failure = xoi::TransportFailure::Callback;
        response.transport_error =
            std::string("stream callback failed: ") + error.what();
      } catch (...) {
        response.response_callback_failed = true;
        response.transport_failure = xoi::TransportFailure::Callback;
        response.transport_error = "stream callback failed";
      }
    }
    return response;
  }

  size_t calls() const {
    return urls.size();
  }

  std::vector<std::string> urls;
  std::vector<std::string> bodies;
  std::vector<std::vector<std::pair<std::string, std::string>>> request_headers;
  std::vector<int> timeouts;
  std::vector<std::size_t> response_limits;

private:
  std::deque<xoi::HttpResponse> responses_;
};

struct VerificationArguments {
  std::string target;
};

void from_json(const nlohmann::json &json, VerificationArguments &arguments) {
  json.at("target").get_to(arguments.target);
}

struct SearchArguments {
  std::string query;
  std::optional<int> limit;
  std::vector<std::string> tags;
  std::array<int, 2> bounds;
  std::string mode;
  double threshold;
  bool case_sensitive;
};

void from_json(const nlohmann::json &json, SearchArguments &arguments) {
  json.at("query").get_to(arguments.query);
  if (json.contains("limit"))
    arguments.limit = json.at("limit").get<int>();
  json.at("tags").get_to(arguments.tags);
  json.at("bounds").get_to(arguments.bounds);
  json.at("mode").get_to(arguments.mode);
  json.at("threshold").get_to(arguments.threshold);
  json.at("case_sensitive").get_to(arguments.case_sensitive);
}

xoi::Config test_config(xoi::Provider provider = xoi::Provider::Anthropic) {
  xoi::Config config;
  config.provider = provider;
  config.api_key = provider == xoi::Provider::Ollama ? "" : "test-key";
  config.base_url =
      provider == xoi::Provider::Anthropic ? "https://anthropic.test"
      : provider == xoi::Provider::Ollama  ? "http://localhost:11434/"
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

TEST(EnvironmentTest, ReadsConfiguredValue) {
  constexpr char name[] = "XOI_TEST_ENVIRONMENT_VALUE";
#ifdef _MSC_VER
  ASSERT_EQ(_putenv_s(name, "configured"), 0);
#else
  ASSERT_EQ(setenv(name, "configured", 1), 0);
#endif

  EXPECT_EQ(xoi::detail::read_environment_variable(name), "configured");

#ifdef _MSC_VER
  EXPECT_EQ(_putenv_s(name, ""), 0);
#else
  EXPECT_EQ(unsetenv(name), 0);
#endif
}

TEST(EnvironmentTest, ReadsProviderHeaderTokenParameterAndEmptyProxyOverride) {
  const std::vector<std::string> names = {"XOI_API_KEY_HEADER",
                                          "XOI_OPENAI_TOKEN_LIMIT_PARAMETER",
                                          "XOI_PROXY_URL"};
  std::vector<std::optional<std::string>> original_values;
  for (const auto &name : names)
    original_values.push_back(
        xoi::detail::read_optional_environment_variable(name.c_str()));

  auto set_environment = [](const std::string &name,
                            const std::optional<std::string> &value) {
#ifdef _MSC_VER
    return _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
    return value ? setenv(name.c_str(), value->c_str(), 1)
                 : unsetenv(name.c_str());
#endif
  };
  ASSERT_EQ(set_environment(names[0], "X-API-Key"), 0);
  ASSERT_EQ(set_environment(names[1], "max_completion_tokens"), 0);
#ifdef _MSC_VER
  ASSERT_EQ(set_environment(names[2], "http://proxy.test"), 0);
#else
  ASSERT_EQ(set_environment(names[2], ""), 0);
#endif

  const auto config = xoi::Config::from_env(xoi::Provider::OpenAICompatible);

  for (std::size_t i = 0; i < names.size(); ++i)
    EXPECT_EQ(set_environment(names[i], original_values[i]), 0);
  EXPECT_EQ(config.api_key_header, "X-API-Key");
  EXPECT_EQ(config.openai_token_limit_parameter,
            xoi::OpenAITokenLimitParameter::MaxCompletionTokens);
  ASSERT_TRUE(config.proxy_url.has_value());
#ifdef _MSC_VER
  EXPECT_EQ(*config.proxy_url, "http://proxy.test");
#else
  EXPECT_TRUE(config.proxy_url->empty());
#endif
}

TEST(EnvironmentTest, OllamaDefaultsToLocalWithoutCredentials) {
  const std::string original_key =
      xoi::detail::read_environment_variable("XOI_API_KEY");
  const std::string original_url =
      xoi::detail::read_environment_variable("XOI_BASE_URL");
#ifdef _MSC_VER
  ASSERT_EQ(_putenv_s("XOI_API_KEY", ""), 0);
  ASSERT_EQ(_putenv_s("XOI_BASE_URL", ""), 0);
#else
  ASSERT_EQ(setenv("XOI_API_KEY", "", 1), 0);
  ASSERT_EQ(setenv("XOI_BASE_URL", "", 1), 0);
#endif

  const auto config = xoi::Config::from_env(xoi::Provider::Ollama);

#ifdef _MSC_VER
  EXPECT_EQ(_putenv_s("XOI_API_KEY", original_key.c_str()), 0);
  EXPECT_EQ(_putenv_s("XOI_BASE_URL", original_url.c_str()), 0);
#else
  if (original_key.empty())
    EXPECT_EQ(unsetenv("XOI_API_KEY"), 0);
  else
    EXPECT_EQ(setenv("XOI_API_KEY", original_key.c_str(), 1), 0);
  if (original_url.empty())
    EXPECT_EQ(unsetenv("XOI_BASE_URL"), 0);
  else
    EXPECT_EQ(setenv("XOI_BASE_URL", original_url.c_str(), 1), 0);
#endif
  EXPECT_TRUE(config.api_key.empty());
  EXPECT_EQ(config.base_url, "http://localhost:11434");
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
  EXPECT_FALSE(sent.contains("temperature"));
  EXPECT_FALSE(sent.contains("top_p"));
  EXPECT_EQ(sent["tools"][0]["input_schema"]["required"][0], "path");
  EXPECT_FALSE(sent.contains("stream"));
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

TEST(ClientTest, SerializesAnthropicSamplingParameters) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"content":[{"type":"text","text":"ok"}]})", {}, {}}});
  auto *observed = mock.get();
  auto request = test_request();
  request.temperature = 0.7;
  request.top_p = 0.9;
  xoi::Client client(test_config(), std::move(mock));

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["temperature"], 0.7);
  EXPECT_EQ(sent["top_p"], 0.9);
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

TEST(ClientTest, RetriesConnectionFailureBeforeResponseBytes) {
  xoi::HttpResponse connection_failure;
  connection_failure.transport_error = "connection refused";
  connection_failure.transport_failure = xoi::TransportFailure::Connection;
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      std::move(connection_failure),
      {200, R"({"content":[{"type":"text","text":"recovered"}]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  EXPECT_EQ(observed->calls(), 2U);
}

TEST(ClientTest, RetriesTimeoutBeforeResponseBytes) {
  xoi::HttpResponse timeout;
  timeout.transport_error = "operation timed out";
  timeout.transport_failure = xoi::TransportFailure::Timeout;
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      std::move(timeout),
      {200, R"({"content":[{"type":"text","text":"recovered"}]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  EXPECT_EQ(observed->calls(), 2U);
}

TEST(ClientTest, DoesNotRetryConnectionFailureAfterResponseBytes) {
  xoi::HttpResponse partial;
  partial.body = "partial";
  partial.response_bytes_received = partial.body.size();
  partial.transport_error = "connection lost";
  partial.transport_failure = xoi::TransportFailure::Connection;
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      std::move(partial), {200, R"({"content":[]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Transport);
  EXPECT_FALSE(result.error().retryable);
  EXPECT_EQ(observed->calls(), 1U);
}

TEST(ClientTest, DoesNotRetryTimeoutAfterResponseBytes) {
  xoi::HttpResponse partial;
  partial.body = "partial";
  partial.response_bytes_received = partial.body.size();
  partial.transport_error = "operation timed out";
  partial.transport_failure = xoi::TransportFailure::Timeout;
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      std::move(partial), {200, R"({"content":[]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Transport);
  EXPECT_FALSE(result.error().retryable);
  EXPECT_EQ(observed->calls(), 1U);
}

TEST(ClientTest, DoesNotRetryOtherTransportFailures) {
  xoi::HttpResponse failure;
  failure.transport_error = "protocol error";
  failure.transport_failure = xoi::TransportFailure::Other;
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      std::move(failure), {200, R"({"content":[]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config();
  config.max_retries = 1;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Transport);
  EXPECT_FALSE(result.error().retryable);
  EXPECT_EQ(observed->calls(), 1U);
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
  EXPECT_EQ(sent["max_tokens"], request.max_tokens);
  EXPECT_FALSE(sent.contains("max_completion_tokens"));
  EXPECT_FALSE(sent.contains("temperature"));
  EXPECT_FALSE(sent.contains("top_p"));
  EXPECT_EQ(observed->request_headers[0].back().first, "Authorization");
  EXPECT_EQ(observed->request_headers[0].back().second, "Bearer test-key");
}

TEST(ClientTest, SerializesOpenAIProviderOptionsAndCustomApiKeyHeader) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"choices":[{"message":{"content":"ok"}}]})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config(xoi::Provider::OpenAICompatible);
  config.api_key_header = "X-API-Key";
  config.openai_token_limit_parameter =
      xoi::OpenAITokenLimitParameter::MaxCompletionTokens;
  auto request = test_request();
  request.max_tokens = 4096;
  request.temperature = 0.7;
  request.top_p = 0.9;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["max_completion_tokens"], 4096);
  EXPECT_FALSE(sent.contains("max_tokens"));
  EXPECT_EQ(sent["temperature"], 0.7);
  EXPECT_EQ(sent["top_p"], 0.9);
  ASSERT_EQ(observed->request_headers[0].size(), 2U);
  EXPECT_EQ(observed->request_headers[0][1].first, "X-API-Key");
  EXPECT_EQ(observed->request_headers[0][1].second, "test-key");
}

TEST(ClientTest, RejectsUnsafeCustomApiKeyHeader) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  auto config = test_config(xoi::Provider::OpenAICompatible);
  config.api_key_header = "X-API-Key\r\nInjected: yes";
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Configuration);
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(ClientTest, RejectsNonFiniteSamplingParameters) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  auto request = test_request();
  request.temperature = std::numeric_limits<double>::quiet_NaN();
  xoi::Client client(test_config(), std::move(mock));

  const auto result = client.send(request);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Configuration);
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(ClientTest, SerializesAndParsesNativeOllamaChat) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"message":{"role":"assistant","content":"Reading file","tool_calls":[{"function":{"name":"read_file","arguments":{"path":"README.md"}}}]},"done":true,"done_reason":"stop"})",
       {},
       {}}});
  auto *observed = mock.get();
  auto request = test_request();
  request.tools.push_back({"read_file",
                           "Read a file",
                           {{"type", "object"},
                            {"properties", {{"path", {{"type", "string"}}}}},
                            {"required", {"path"}}}});
  xoi::Client client(test_config(xoi::Provider::Ollama), std::move(mock));

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().content.size(), 2U);
  EXPECT_EQ(std::get<xoi::Text>(result.value().content[0]).text,
            "Reading file");
  const auto &call = std::get<xoi::ToolUse>(result.value().content[1]);
  EXPECT_EQ(call.id, "ollama-tool-0");
  EXPECT_EQ(call.name, "read_file");
  EXPECT_EQ(call.input["path"], "README.md");
  EXPECT_EQ(result.value().stop_reason, "stop");
  ASSERT_EQ(observed->urls[0], "http://localhost:11434/api/chat");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["messages"][0]["role"], "system");
  EXPECT_EQ(sent["messages"][0]["content"], "Be concise.");
  EXPECT_EQ(sent["messages"][1]["role"], "user");
  EXPECT_EQ(sent["tools"][0]["function"]["name"], "read_file");
  EXPECT_EQ(sent["tools"][0]["function"]["parameters"]["required"][0], "path");
  EXPECT_EQ(sent["options"]["num_predict"], request.max_tokens);
  EXPECT_FALSE(sent["options"].contains("temperature"));
  EXPECT_FALSE(sent["options"].contains("top_p"));
  EXPECT_FALSE(sent["stream"].get<bool>());
  EXPECT_TRUE(observed->request_headers[0].empty() ||
              std::none_of(observed->request_headers[0].begin(),
                           observed->request_headers[0].end(),
                           [](const auto &header) {
                             return header.first == "Authorization";
                           }));
}

TEST(ClientTest, SerializesOllamaSamplingParameters) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"message":{"content":"ok"}})", {}, {}}});
  auto *observed = mock.get();
  auto request = test_request();
  request.temperature = 0.2;
  request.top_p = 0.8;
  xoi::Client client(test_config(xoi::Provider::Ollama), std::move(mock));

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["options"]["temperature"], 0.2);
  EXPECT_EQ(sent["options"]["top_p"], 0.8);
}

TEST(ClientTest, CanOmitOllamaPredictionLimitToUseProviderDefault) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200, R"({"message":{"content":"ok"}})", {}, {}}});
  auto *observed = mock.get();
  auto config = test_config(xoi::Provider::Ollama);
  config.ollama_send_num_predict = false;
  xoi::Client client(std::move(config), std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_FALSE(sent.contains("options"));
  EXPECT_FALSE(sent["stream"].get<bool>());
}

TEST(ClientTest, SerializesOllamaToolResultsAsToolMessages) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"message":{"role":"assistant","content":"done"},"done":true})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(xoi::Provider::Ollama), std::move(mock));
  auto request = test_request();
  request.messages = {
      {"assistant",
       {xoi::ToolUse{"internal-id", "read_file", {{"path", "README.md"}}}}},
      {"user", {xoi::ToolResult{"internal-id", "file contents", false}}}};

  const auto result = client.send(request);

  ASSERT_TRUE(result);
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_EQ(sent["messages"][1]["role"], "assistant");
  EXPECT_EQ(
      sent["messages"][1]["tool_calls"][0]["function"]["arguments"]["path"],
      "README.md");
  EXPECT_EQ(sent["messages"][2]["role"], "tool");
  EXPECT_EQ(sent["messages"][2]["content"], "file contents");
}

TEST(ClientTest, StreamsNativeOllamaNdjsonTextAndToolCalls) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello \"},"
       "\"done\":false}\n"
       "{\"message\":{\"role\":\"assistant\",\"content\":\"world\","
       "\"tool_calls\":[{\"function\":{\"name\":\"read_file\","
       "\"arguments\":{\"path\":\"README.md\"}}}]},\"done\":false}\n"
       "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
       "\"done_reason\":\"stop\",\"done\":true}\n",
       {},
       {}}});
  auto *observed = mock.get();
  auto config = test_config(xoi::Provider::Ollama);
  xoi::Client client(config, std::move(mock));
  auto request = test_request();
  std::string streamed_text;

  const auto result =
      client.send_stream(request, [&streamed_text](std::string_view delta) {
        streamed_text.append(delta);
      });

  ASSERT_TRUE(result);
  EXPECT_EQ(streamed_text, "Hello world");
  ASSERT_EQ(result.value().content.size(), 2U);
  EXPECT_EQ(std::get<xoi::Text>(result.value().content[0]).text, "Hello world");
  const auto &call = std::get<xoi::ToolUse>(result.value().content[1]);
  EXPECT_EQ(call.name, "read_file");
  EXPECT_EQ(call.input["path"], "README.md");
  EXPECT_EQ(result.value().stop_reason, "stop");
  EXPECT_EQ(observed->urls[0], "http://localhost:11434/api/chat");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_TRUE(sent["stream"].get<bool>());
  EXPECT_EQ(observed->request_headers[0].back().second, "application/x-ndjson");
}

TEST(ClientTest, RejectsMalformedOllamaNdjson) {
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, "{\"message\":\n", {}, {}}});
  xoi::Client client(test_config(xoi::Provider::Ollama), std::move(mock));

  const auto result =
      client.send_stream(test_request(), [](std::string_view) {});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_NE(result.error().message.find("invalid Ollama streaming event"),
            std::string::npos);
}

TEST(AgentTest, RunsNativeOllamaToolCycle) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"read_file","arguments":{"path":"README.md"}}}]},"done":true})",
       {},
       {}},
      {200,
       R"({"message":{"role":"assistant","content":"The file says hello."},"done":true,"done_reason":"stop"})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(xoi::Provider::Ollama), std::move(mock));
  xoi::ToolRegistry tools;
  EXPECT_TRUE(tools
                  .add({"read_file",
                        "Read a file",
                        {{"type", "object"},
                         {"properties", {{"path", {{"type", "string"}}}}},
                         {"required", {"path"}}}},
                       [](const nlohmann::json &input) {
                         return xoi::Result<std::string>(
                             "contents of " + input["path"].get<std::string>());
                       })
                  .empty());

  const auto result = xoi::run_agent(client, test_request(), tools);

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().text, "The file says hello.");
  EXPECT_EQ(result.value().tool_calls, 1);
  const auto follow_up = nlohmann::json::parse(observed->bodies[1]);
  EXPECT_EQ(follow_up["messages"][2]["tool_calls"][0]["function"]["name"],
            "read_file");
  EXPECT_EQ(follow_up["messages"][3]["role"], "tool");
  EXPECT_EQ(follow_up["messages"][3]["content"], "contents of README.md");
}

TEST(ClientTest, StreamsAnthropicTextAcrossTransportChunkBoundaries) {
  const std::string events =
      "event: message_start\r\n"
      "data: "
      "{\"type\":\"message_start\",\"message\":{\"stop_reason\":null}}\r\n\r\n"
      "event: content_block_start\r\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
      "\"type\":\"text\",\"text\":\"\"}}\r\n\r\n"
      "event: content_block_delta\r\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
      "\"text_delta\",\"text\":\"Hello\"}}\r\n\r\n"
      "event: content_block_delta\r\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
      "\"text_delta\",\"text\":\" world\"}}\r\n\r\n"
      "event: message_delta\r\n"
      "data: "
      "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}"
      "\r\n\r\n"
      "event: message_stop\r\n"
      "data: {\"type\":\"message_stop\"}\r\n\r\n";
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, events, {}, {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  std::string streamed_text;

  const auto result = client.send_stream(
      test_request(),
      [&streamed_text](std::string_view text) { streamed_text.append(text); });

  ASSERT_TRUE(result);
  EXPECT_EQ(streamed_text, "Hello world");
  ASSERT_EQ(result.value().content.size(), 1U);
  EXPECT_EQ(std::get<xoi::Text>(result.value().content[0]).text, "Hello world");
  EXPECT_EQ(result.value().stop_reason, "end_turn");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_TRUE(sent["stream"].get<bool>());
  EXPECT_NE(std::find(observed->request_headers[0].begin(),
                      observed->request_headers[0].end(),
                      std::make_pair(std::string("Accept"),
                                     std::string("text/event-stream"))),
            observed->request_headers[0].end());
  EXPECT_EQ(observed->urls[0], "https://anthropic.test/v1/messages");
}

TEST(ClientTest, StreamsAnthropicToolCalls) {
  const std::string events =
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
      "\"type\":\"tool_use\",\"id\":\"tool-1\",\"name\":\"read_file\","
      "\"input\":{}}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
      "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\"}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
      "\"input_json_delta\",\"partial_json\":\"\\\"README.md\\\"}\"}}\n\n"
      "event: message_delta\n"
      "data: "
      "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}"
      "\n\n";
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, events, {}, {}}});
  xoi::Client client(test_config(), std::move(mock));
  std::string streamed_text;

  const auto result = client.send_stream(
      test_request(),
      [&streamed_text](std::string_view text) { streamed_text.append(text); });

  ASSERT_TRUE(result);
  EXPECT_TRUE(streamed_text.empty());
  ASSERT_EQ(result.value().content.size(), 1U);
  const auto &tool = std::get<xoi::ToolUse>(result.value().content[0]);
  EXPECT_EQ(tool.id, "tool-1");
  EXPECT_EQ(tool.name, "read_file");
  EXPECT_EQ(tool.input["path"], "README.md");
  EXPECT_EQ(result.value().stop_reason, "tool_use");
}

TEST(ClientTest, StreamsOpenAITextAndAssemblesToolCalls) {
  const std::string events =
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Reading "
      "\"}}]}\n\n"
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"file\"}}]}\n\n"
      "data: "
      "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
      "\"id\":\"call-1\",\"function\":{\"name\":\"read_\",\"arguments\":\"{"
      "\\\"path\\\":\"}}]}}]}\n\n"
      "data: "
      "{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
      "\"function\":{\"name\":\"file\",\"arguments\":\"\\\"README.md\\\"}\"}}]}"
      "}]}\n\n"
      "data: "
      "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_"
      "calls\"}]}\n\n"
      "data: [DONE]\n\n";
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, events, {}, {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));
  std::string streamed_text;

  const auto result = client.send_stream(
      test_request(),
      [&streamed_text](std::string_view text) { streamed_text.append(text); });

  ASSERT_TRUE(result);
  EXPECT_EQ(streamed_text, "Reading file");
  ASSERT_EQ(result.value().content.size(), 2U);
  EXPECT_EQ(std::get<xoi::Text>(result.value().content[0]).text,
            "Reading file");
  const auto &tool = std::get<xoi::ToolUse>(result.value().content[1]);
  EXPECT_EQ(tool.id, "call-1");
  EXPECT_EQ(tool.name, "read_file");
  EXPECT_EQ(tool.input["path"], "README.md");
  EXPECT_EQ(result.value().stop_reason, "tool_calls");
  const auto sent = nlohmann::json::parse(observed->bodies[0]);
  EXPECT_TRUE(sent["stream"].get<bool>());
  EXPECT_EQ(observed->urls[0], "https://openai.test/v1/chat/completions");
}

TEST(ClientTest, ReportsStreamingCallbackExceptions) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n",
       {},
       {}}});
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));

  const auto result = client.send_stream(test_request(), [](std::string_view) {
    throw std::runtime_error("consumer stopped");
  });

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Callback);
  EXPECT_NE(result.error().message.find("consumer stopped"), std::string::npos);
}

TEST(ClientTest, ReportsMalformedStreamingEvents) {
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, "data: {invalid}\n\n", {}, {}}});
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));

  const auto result =
      client.send_stream(test_request(), [](std::string_view) {});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_NE(result.error().message.find("invalid streaming event"),
            std::string::npos);
}

TEST(ClientTest, RejectsInvalidStreamingEventShape) {
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200,
                                      "data: "
                                      "{\"choices\":\"not-an-array\"}\n\n",
                                      {},
                                      {}}});
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));

  const auto result =
      client.send_stream(test_request(), [](std::string_view) {});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_EQ(result.error().message, "streaming event choices must be an array");
}

TEST(ClientTest, ReportsProviderStreamingErrors) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       "data: "
       "{\"error\":{\"message\":\"overloaded\"}}\n\n",
       {},
       {}}});
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));

  const auto result =
      client.send_stream(test_request(), [](std::string_view) {});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_NE(result.error().message.find("overloaded"), std::string::npos);
}

TEST(ClientTest, RejectsEmptyStreamingResponse) {
  auto mock = std::make_unique<MockTransport>(
      std::vector<xoi::HttpResponse>{{200, {}, {}, {}}});
  xoi::Client client(test_config(xoi::Provider::OpenAICompatible),
                     std::move(mock));

  const auto result =
      client.send_stream(test_request(), [](std::string_view) {});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_EQ(result.error().message, "stream response contained no events");
}

TEST(ClientTest, EnumJsonSerializationPreservesUnderlyingValues) {
  EXPECT_EQ(nlohmann::json(xoi::Provider::Anthropic), 0);
  EXPECT_EQ(nlohmann::json(xoi::Provider::OpenAICompatible), 1);
  EXPECT_EQ(nlohmann::json(xoi::ErrorKind::Configuration), 0);
  EXPECT_EQ(nlohmann::json(xoi::ErrorKind::Limit), 4);
  EXPECT_EQ(nlohmann::json(xoi::TransportFailure::Connection), 6);
  EXPECT_EQ(static_cast<unsigned int>(xoi::Provider::OpenAICompatible), 1U);
  EXPECT_EQ(static_cast<unsigned int>(xoi::ErrorKind::Limit), 4U);
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
  auto *observed = mock.get();
  auto config = test_config();
  config.max_response_bytes = 12;
  xoi::Client client(config, std::move(mock));

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::ResponseTooLarge);
  EXPECT_FALSE(result.error().retryable);
  ASSERT_EQ(observed->response_limits.size(), 1U);
  EXPECT_EQ(observed->response_limits[0], 12U);
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

TEST(CurlBodyCallbackTest, ReturnsZeroAndMarksFailureWhenAppendThrows) {
  xoi::detail::BodyCallbackState state;
  state.max_bytes = 16;
  state.append = [](std::string &, std::string_view) {
    throw std::bad_alloc();
  };
  char chunk[] = "body";

  EXPECT_EQ(xoi::detail::append_body(chunk, 1, 4, &state), 0U);
  EXPECT_TRUE(state.callback_failed);
  EXPECT_FALSE(state.too_large);
}

TEST(CurlBodyCallbackTest, StopsBeforeAppendingBytesAboveLimit) {
  xoi::detail::BodyCallbackState state;
  state.max_bytes = 3;
  char chunk[] = "body";

  EXPECT_EQ(xoi::detail::append_body(chunk, 1, 4, &state), 0U);
  EXPECT_TRUE(state.too_large);
  EXPECT_FALSE(state.callback_failed);
  EXPECT_TRUE(state.body.empty());
}

TEST(CurlBodyCallbackTest, CountsBytesForwardedWithoutBuffering) {
  xoi::detail::BodyCallbackState state;
  state.max_bytes = 16;
  state.append = [](std::string &, std::string_view) {};
  char chunk[] = "stream";

  EXPECT_EQ(xoi::detail::append_body(chunk, 1, 6, &state), 6U);
  EXPECT_TRUE(state.body.empty());
  EXPECT_EQ(state.received_bytes, 6U);
}

TEST(CurlOptionTest, ReportsInvalidOptionInsteadOfIgnoringIt) {
  CURL *handle = curl_easy_init();
  ASSERT_NE(handle, nullptr);

  constexpr auto unknown_option =
      static_cast<CURLoption>(CURLOPTTYPE_LONG + 9999);
  const CURLcode code =
      xoi::detail::checked_set_option(handle, unknown_option, 0L);

  EXPECT_NE(code, CURLE_OK);
  curl_easy_cleanup(handle);
}

TEST(ClientTest, MapsCurlTransferFailureToTypedTransportError) {
  auto config = test_config();
  config.base_url = "not a valid URL";
  config.max_retries = 0;
  xoi::Client client(config);

  const auto result = client.send(test_request());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Transport);
  EXPECT_EQ(result.error().transport_failure, xoi::TransportFailure::Other);
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

TEST(ToolRegistryTest, DecodesTypedArgumentsAfterSchemaValidation) {
  xoi::ToolRegistry tools;
  bool handler_called = false;
  EXPECT_TRUE(tools
                  .add_typed<VerificationArguments>(
                      {"verify",
                       "Return verification output for a target",
                       {{"type", "object"},
                        {"properties", {{"target", {{"type", "string"}}}}},
                        {"required", {"target"}}}},
                      [&handler_called](const VerificationArguments &input)
                          -> xoi::Result<std::string> {
                        handler_called = true;
                        return "verified " + input.target;
                      })
                  .empty());

  const auto result =
      tools.execute("verify", nlohmann::json{{"target", "tests"}});

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), "verified tests");
  EXPECT_TRUE(handler_called);
  ASSERT_EQ(tools.specs().size(), 1U);
  EXPECT_EQ(tools.specs()[0].input_schema["required"][0], "target");
}

TEST(ToolRegistryTest, InfersTypedFieldSchemasAndValidatesBeforeHandler) {
  xoi::ToolRegistry tools;
  int handler_calls = 0;
  EXPECT_TRUE(
      tools
          .add_typed<SearchArguments>(
              "search", "Search with typed arguments",
              [&handler_calls](const SearchArguments &arguments)
                  -> xoi::Result<std::string> {
                ++handler_calls;
                return arguments.query + ":" + arguments.mode;
              },
              xoi::schema_field("query", &SearchArguments::query),
              xoi::schema_field("limit", &SearchArguments::limit),
              xoi::schema_field("tags", &SearchArguments::tags),
              xoi::schema_field("bounds", &SearchArguments::bounds),
              xoi::schema_field(
                  "mode", &SearchArguments::mode,
                  {{"type", "string"}, {"enum", {"safe", "fast"}}}),
              xoi::schema_field("threshold", &SearchArguments::threshold),
              xoi::schema_field("case_sensitive",
                                &SearchArguments::case_sensitive))
          .empty());

  ASSERT_EQ(tools.specs().size(), 1U);
  const auto &schema = tools.specs()[0].input_schema;
  EXPECT_EQ(schema["type"], "object");
  EXPECT_EQ(schema["additionalProperties"], false);
  EXPECT_EQ(schema["properties"]["query"],
            (nlohmann::json{{"type", "string"}}));
  EXPECT_EQ(schema["properties"]["limit"],
            (nlohmann::json{{"type", "integer"}}));
  EXPECT_EQ(schema["properties"]["tags"]["items"],
            (nlohmann::json{{"type", "string"}}));
  EXPECT_EQ(schema["properties"]["bounds"]["minItems"], 2);
  EXPECT_EQ(schema["properties"]["bounds"]["maxItems"], 2);
  EXPECT_EQ(schema["properties"]["mode"]["enum"],
            nlohmann::json({"safe", "fast"}));
  EXPECT_EQ(schema["properties"]["threshold"]["type"], "number");
  EXPECT_EQ(schema["properties"]["case_sensitive"]["type"], "boolean");
  EXPECT_EQ(schema["required"],
            nlohmann::json({"query", "tags", "bounds", "mode", "threshold",
                            "case_sensitive"}));

  const auto valid = tools.execute("search", {{"query", "schema"},
                                              {"tags", {"c++"}},
                                              {"bounds", {1, 5}},
                                              {"mode", "safe"},
                                              {"threshold", 0.5},
                                              {"case_sensitive", true}});
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid.value(), "schema:safe");
  const auto valid_with_optional =
      tools.execute("search", {{"query", "schema"},
                               {"limit", 10},
                               {"tags", {"c++"}},
                               {"bounds", {1, 5}},
                               {"mode", "safe"},
                               {"threshold", 0.5},
                               {"case_sensitive", true}});
  ASSERT_TRUE(valid_with_optional);

  const auto invalid_enum = tools.execute("search", {{"query", "schema"},
                                                     {"tags", {"c++"}},
                                                     {"bounds", {1, 5}},
                                                     {"mode", "unsafe"},
                                                     {"threshold", 0.5},
                                                     {"case_sensitive", true}});
  ASSERT_FALSE(invalid_enum);
  EXPECT_EQ(invalid_enum.error().message,
            "argument 'mode' must be one of the declared enum values");

  const auto unknown_field = tools.execute("search", {{"query", "schema"},
                                                      {"tags", {"c++"}},
                                                      {"bounds", {1, 5}},
                                                      {"mode", "safe"},
                                                      {"threshold", 0.5},
                                                      {"case_sensitive", true},
                                                      {"extra", true}});
  ASSERT_FALSE(unknown_field);
  EXPECT_EQ(unknown_field.error().message, "argument 'extra' is not allowed");
  EXPECT_EQ(handler_calls, 2);
}

TEST(ToolRegistryTest, RejectsDuplicateTypedSchemaFieldNames) {
  xoi::ToolRegistry tools;
  const auto error = tools.add_typed<VerificationArguments>(
      "verify", "Verify a target",
      [](const VerificationArguments &) -> xoi::Result<std::string> {
        return std::string{"verified"};
      },
      xoi::schema_field("target", &VerificationArguments::target),
      xoi::schema_field("target", &VerificationArguments::target));

  EXPECT_EQ(error, "tool schema field names must be unique");
  EXPECT_TRUE(tools.specs().empty());
}

TEST(ToolRegistryTest, ReturnsTypedDeserializationFailuresAsToolErrors) {
  xoi::ToolRegistry tools;
  bool handler_called = false;
  EXPECT_TRUE(
      tools
          .add_typed<VerificationArguments>(
              {"verify", "Return verification output", {{"type", "object"}}},
              [&handler_called](
                  const VerificationArguments &) -> xoi::Result<std::string> {
                handler_called = true;
                return std::string{"unexpected"};
              })
          .empty());

  const auto result = tools.execute("verify", nlohmann::json{{"target", 42}});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Protocol);
  EXPECT_EQ(result.error().message.substr(
                0, std::string("tool arguments could not be decoded").size()),
            "tool arguments could not be decoded");
  EXPECT_FALSE(handler_called);
}

TEST(ToolRegistryTest, ValidatesNestedObjectsAndArrayItems) {
  xoi::ToolRegistry tools;
  bool handler_called = false;
  const nlohmann::json schema = {
      {"type", "object"},
      {"properties",
       {{"settings",
         {{"type", "object"},
          {"properties", {{"mode", {{"type", "string"}, {"enum", {"safe"}}}}}},
          {"required", {"mode"}},
          {"additionalProperties", false}}},
        {"limits",
         {{"type", "array"},
          {"items",
           {{"type", "object"},
            {"properties", {{"count", {{"type", "integer"}}}}},
            {"required", {"count"}}}}}}}},
      {"required", {"settings", "limits"}}};
  EXPECT_TRUE(tools
                  .add({"configure", "Apply configuration", schema},
                       [&handler_called](
                           const nlohmann::json &) -> xoi::Result<std::string> {
                         handler_called = true;
                         return std::string{"configured"};
                       })
                  .empty());

  const auto result = tools.execute(
      "configure", {{"settings", {{"mode", "safe"}}},
                    {"limits", nlohmann::json::array({{{"count", 2}}})}});

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), "configured");
  EXPECT_TRUE(handler_called);
}

TEST(ToolRegistryTest, RejectsInvalidNestedValuesAndUnknownProperties) {
  xoi::ToolRegistry tools;
  bool handler_called = false;
  const nlohmann::json schema = {
      {"type", "object"},
      {"properties",
       {{"settings",
         {{"type", "object"},
          {"properties", {{"count", {{"type", "integer"}}}}},
          {"required", {"count"}},
          {"additionalProperties", false}}}}}};
  EXPECT_TRUE(tools
                  .add({"configure", "Apply configuration", schema},
                       [&handler_called](
                           const nlohmann::json &) -> xoi::Result<std::string> {
                         handler_called = true;
                         return std::string{"unexpected"};
                       })
                  .empty());

  const auto invalid_type =
      tools.execute("configure", {{"settings", {{"count", "many"}}}});
  ASSERT_FALSE(invalid_type);
  EXPECT_EQ(invalid_type.error().message,
            "argument 'settings.count' must be integer");
  EXPECT_FALSE(handler_called);

  const auto missing_required =
      tools.execute("configure", {{"settings", nlohmann::json::object()}});
  ASSERT_FALSE(missing_required);
  EXPECT_EQ(missing_required.error().message,
            "missing required argument 'settings.count'");
  EXPECT_FALSE(handler_called);

  const auto extra_property = tools.execute(
      "configure", {{"settings", {{"count", 2}, {"extra", true}}}});
  ASSERT_FALSE(extra_property);
  EXPECT_EQ(extra_property.error().message,
            "argument 'settings.extra' is not allowed");
  EXPECT_FALSE(handler_called);
}

TEST(ToolRegistryTest, RejectsInvalidArrayItemsAndEnumValues) {
  xoi::ToolRegistry tools;
  bool handler_called = false;
  const nlohmann::json schema = {
      {"type", "object"},
      {"properties",
       {{"mode", {{"type", "string"}, {"enum", {"safe", "strict"}}}},
        {"counts", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}}};
  EXPECT_TRUE(tools
                  .add({"configure", "Apply configuration", schema},
                       [&handler_called](
                           const nlohmann::json &) -> xoi::Result<std::string> {
                         handler_called = true;
                         return std::string{"unexpected"};
                       })
                  .empty());

  const auto invalid_enum =
      tools.execute("configure", {{"mode", "unsafe"}, {"counts", {1, 2}}});
  ASSERT_FALSE(invalid_enum);
  EXPECT_EQ(invalid_enum.error().message,
            "argument 'mode' must be one of the declared enum values");

  const auto invalid_item =
      tools.execute("configure", {{"mode", "safe"}, {"counts", {1, "two"}}});
  ASSERT_FALSE(invalid_item);
  EXPECT_EQ(invalid_item.error().message,
            "argument 'counts[1]' must be integer");
  EXPECT_FALSE(handler_called);
}

TEST(ToolRegistryTest, ValidatesNumericStringAndArrayBoundsRecursively) {
  xoi::ToolRegistry tools;
  int handler_calls = 0;
  const nlohmann::json schema = {
      {"type", "object"},
      {"properties",
       {{"name", {{"type", "string"}, {"minLength", 2}, {"maxLength", 3}}},
        {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}},
        {"ratio", {{"type", "number"}, {"minimum", 1.5}, {"maximum", 2.5}}},
        {"tags",
         {{"type", "array"},
          {"minItems", 1},
          {"maxItems", 2},
          {"items",
           {{"type", "string"}, {"minLength", 2}, {"maxLength", 3}}}}}}}};
  EXPECT_TRUE(tools
                  .add({"bounded", "Validate bounded values", schema},
                       [&handler_calls](
                           const nlohmann::json &) -> xoi::Result<std::string> {
                         ++handler_calls;
                         return std::string{"accepted"};
                       })
                  .empty());

  const auto minimums = tools.execute(
      "bounded",
      {{"name", "猫a"}, {"count", 1}, {"ratio", 1.5}, {"tags", {"ok"}}});
  ASSERT_TRUE(minimums);
  EXPECT_EQ(minimums.value(), "accepted");
  const auto maximums = tools.execute("bounded", {{"name", "abc"},
                                                  {"count", 5},
                                                  {"ratio", 2.5},
                                                  {"tags", {"one", "two"}}});
  ASSERT_TRUE(maximums);
  EXPECT_EQ(maximums.value(), "accepted");

  const std::vector<std::pair<nlohmann::json, std::string>> invalid_inputs = {
      {{{"name", "猫"}, {"count", 1}, {"ratio", 2.0}, {"tags", {"one"}}},
       "argument 'name' must have at least 2 characters"},
      {{{"name", "abcd"}, {"count", 1}, {"ratio", 2.0}, {"tags", {"one"}}},
       "argument 'name' must have at most 3 characters"},
      {{{"name", "ab"}, {"count", 0}, {"ratio", 2.0}, {"tags", {"one"}}},
       "argument 'count' must be >= 1"},
      {{{"name", "ab"}, {"count", 6}, {"ratio", 2.0}, {"tags", {"one"}}},
       "argument 'count' must be <= 5"},
      {{{"name", "ab"},
        {"count", 1},
        {"ratio", 2.0},
        {"tags", nlohmann::json::array()}},
       "argument 'tags' must contain at least 1 items"},
      {{{"name", "ab"},
        {"count", 1},
        {"ratio", 2.0},
        {"tags", {"one", "two", "three"}}},
       "argument 'tags' must contain at most 2 items"},
      {{{"name", "ab"}, {"count", 1}, {"ratio", 1.4}, {"tags", {"one"}}},
       "argument 'ratio' must be >= 1.5"},
      {{{"name", "ab"}, {"count", 1}, {"ratio", 2.6}, {"tags", {"one"}}},
       "argument 'ratio' must be <= 2.5"},
      {{{"name", "ab"}, {"count", 1}, {"ratio", 2.0}, {"tags", {"x"}}},
       "argument 'tags[0]' must have at least 2 characters"}};
  for (const auto &[input, error] : invalid_inputs) {
    const auto result = tools.execute("bounded", input);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message, error);
  }
  EXPECT_EQ(handler_calls, 2);
}

TEST(ToolRegistryTest, RejectsMalformedOrUnsupportedSchemaKeywords) {
  xoi::ToolRegistry tools;
  const auto handler = [](const nlohmann::json &) {
    return xoi::Result<std::string>(std::string{"unused"});
  };

  EXPECT_EQ(tools.add({"bad_type", "Bad type", {{"type", "decimal"}}}, handler),
            "tool input schema type must be a supported JSON type");
  EXPECT_EQ(tools.add({"bad_items",
                       "Bad items",
                       {{"type", "object"},
                        {"properties", {{"values", {{"items", 1}}}}}}},
                      handler),
            "tool input schema.properties.values.items must be an object");
  EXPECT_EQ(tools.add({"bad_enum",
                       "Bad enum",
                       {{"type", "string"}, {"enum", nlohmann::json::array()}}},
                      handler),
            "tool input schema enum must be a non-empty array");
  EXPECT_EQ(tools.add({"bad_additional",
                       "Bad additional",
                       {{"type", "object"}, {"additionalProperties", "no"}}},
                      handler),
            "tool input schema additionalProperties must be a boolean");
  EXPECT_EQ(tools.add({"bad_minimum",
                       "Bad minimum",
                       {{"type", "number"}, {"minimum", "1"}}},
                      handler),
            "tool input schema minimum must be a finite number");
  EXPECT_EQ(tools.add({"bad_max_length",
                       "Bad maximum length",
                       {{"type", "string"}, {"maxLength", -1}}},
                      handler),
            "tool input schema maxLength must be a non-negative integer");
  EXPECT_EQ(tools.add({"bad_min_items",
                       "Bad minimum items",
                       {{"type", "array"}, {"minItems", 1.5}}},
                      handler),
            "tool input schema minItems must be a non-negative integer");
  EXPECT_EQ(tools.add({"ignored_constraint",
                       "Ignored constraint",
                       {{"type", "object"}, {"minProperties", 1}}},
                      handler),
            "tool input schema uses unsupported keyword 'minProperties'");
  EXPECT_TRUE(tools.specs().empty());
}

TEST(AgentTest, ReturnsTypedDeserializationErrorsToTheModel) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"verify","input":{"target":42}}],"stop_reason":"tool_use"})",
       {},
       {}},
      {200,
       R"({"content":[{"type":"text","text":"The tool input was invalid."}]})",
       {},
       {}}});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  EXPECT_TRUE(
      tools
          .add_typed<VerificationArguments>(
              {"verify", "Return verification output", {{"type", "object"}}},
              [](const VerificationArguments &) -> xoi::Result<std::string> {
                return std::string{"unexpected"};
              })
          .empty());

  const auto result = xoi::run_agent(client, test_request(), tools);

  ASSERT_TRUE(result);
  const auto follow_up = nlohmann::json::parse(observed->bodies[1]);
  EXPECT_TRUE(follow_up["messages"][2]["content"][0]["is_error"].get<bool>());
  EXPECT_EQ(
      follow_up["messages"][2]["content"][0]["content"]
          .get<std::string>()
          .substr(0, std::string("tool arguments could not be decoded").size()),
      "tool arguments could not be decoded");
}

TEST(AgentTest, CancelsBeforeMakingAProviderRequest) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  std::stop_source stop_source;
  stop_source.request_stop();
  xoi::AgentOptions options;
  options.stop_token = stop_source.get_token();

  const auto result = xoi::run_agent(client, test_request(), tools, options);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Cancelled);
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(AgentTest, ReportsOrderedProgressWithoutToolArgumentsOrResults) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"read_file","input":{"path":"private.txt"}}]})",
       {},
       {}},
      {200, R"({"content":[{"type":"text","text":"done"}]})", {}, {}}});
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  EXPECT_TRUE(tools
                  .add({"read_file", "Read a file", {{"type", "object"}}},
                       [](const nlohmann::json &) {
                         return xoi::Result<std::string>("private contents");
                       })
                  .empty());
  std::vector<xoi::AgentEvent> events;
  xoi::AgentOptions options;
  options.on_event = [&events](const xoi::AgentEvent &event) {
    events.push_back(event);
  };

  const auto result = xoi::run_agent(client, test_request(), tools, options);

  ASSERT_TRUE(result);
  ASSERT_EQ(events.size(), 6U);
  EXPECT_EQ(events[0].kind, xoi::AgentEventKind::ModelRequestStarted);
  EXPECT_EQ(events[0].step, 1);
  EXPECT_EQ(events[1].kind, xoi::AgentEventKind::ModelResponseReceived);
  EXPECT_EQ(events[2].kind, xoi::AgentEventKind::ToolExecutionStarted);
  EXPECT_EQ(events[2].tool_name, "read_file");
  EXPECT_EQ(events[3].kind, xoi::AgentEventKind::ToolExecutionCompleted);
  EXPECT_FALSE(events[3].is_error);
  EXPECT_EQ(events[4].kind, xoi::AgentEventKind::ModelRequestStarted);
  EXPECT_EQ(events[4].step, 2);
  EXPECT_EQ(events[5].kind, xoi::AgentEventKind::ModelResponseReceived);
  EXPECT_TRUE(events[2].tool_name.find("private") == std::string::npos);
}

TEST(AgentTest, CancellationFromProgressCallbackPreventsToolExecution) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"noop","input":{}}]})",
       {},
       {}}});
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  int executions = 0;
  EXPECT_TRUE(tools
                  .add({"noop", "No operation", {{"type", "object"}}},
                       [&executions](const nlohmann::json &) {
                         ++executions;
                         return xoi::Result<std::string>("done");
                       })
                  .empty());
  std::stop_source stop_source;
  xoi::AgentOptions options;
  options.stop_token = stop_source.get_token();
  options.on_event = [&stop_source](const xoi::AgentEvent &event) {
    if (event.kind == xoi::AgentEventKind::ToolExecutionStarted)
      stop_source.request_stop();
  };

  const auto result = xoi::run_agent(client, test_request(), tools, options);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Cancelled);
  EXPECT_EQ(executions, 0);
}

TEST(AgentTest, ReportsProgressCallbackExceptions) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{});
  auto *observed = mock.get();
  xoi::Client client(test_config(), std::move(mock));
  xoi::ToolRegistry tools;
  xoi::AgentOptions options;
  options.on_event = [](const xoi::AgentEvent &) {
    throw std::runtime_error("observer failed");
  };

  const auto result = xoi::run_agent(client, test_request(), tools, options);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, xoi::ErrorKind::Callback);
  EXPECT_EQ(result.error().message,
            "agent progress callback failed: observer failed");
  EXPECT_EQ(observed->calls(), 0U);
}

TEST(AgentTest, ReturnsArgumentErrorsAsToolResultObservations) {
  auto mock = std::make_unique<MockTransport>(std::vector<xoi::HttpResponse>{
      {200,
       R"({"content":[{"type":"tool_use","id":"call-1","name":"read_file","input":{"config":{"path":7}}}],"stop_reason":"tool_use"})",
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
                         {"properties",
                          {{"config",
                            {{"type", "object"},
                             {"properties", {{"path", {{"type", "string"}}}}},
                             {"required", {"path"}}}}}},
                         {"required", {"config"}}}},
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
            "argument 'config.path' must be string");
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

  xoi::AgentOptions options;
  options.max_steps = 1;
  options.max_tool_calls = 2;
  const auto result = xoi::run_agent(client, test_request(), tools, options);

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
