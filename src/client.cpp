#include <xoi/client.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace xoi {
namespace {

std::string environment_value(const char *name) {
  const char *value = std::getenv(name);
  return value ? value : "";
}

std::string strip_trailing_slashes(std::string value) {
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

std::string endpoint(const Config &config) {
  const std::string base = strip_trailing_slashes(config.base_url);
  if (config.provider == Provider::Anthropic)
    return base + "/v1/messages";
  return base + "/chat/completions";
}

std::string error_message(const HttpResponse &response) {
  if (nlohmann::json::accept(response.body)) {
    const auto parsed = nlohmann::json::parse(response.body);
    if (parsed.contains("error")) {
      if (parsed["error"].is_object() && parsed["error"].contains("message") &&
          parsed["error"]["message"].is_string())
        return parsed["error"]["message"].get<std::string>();
      if (parsed["error"].is_string())
        return parsed["error"].get<std::string>();
    }
    if (parsed.contains("message") && parsed["message"].is_string())
      return parsed["message"].get<std::string>();
  }
  return response.body.empty() ? "HTTP request failed" : response.body;
}

long retry_after_seconds(const HttpResponse &response) {
  const std::string value = response.header("Retry-After");
  if (value.empty())
    return -1;
  long seconds = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error == std::errc{} && end == value.data() + value.size())
    return std::max(seconds, 0L);

  const time_t retry_at = curl_getdate(value.c_str(), nullptr);
  if (retry_at < 0)
    return -1;
  return std::max(static_cast<long>(retry_at - std::time(nullptr)), 0L);
}

Error protocol_error(std::string message) {
  return {ErrorKind::Protocol, std::move(message), 0, false};
}

nlohmann::json anthropic_request_json(const Request &request) {
  nlohmann::json messages = nlohmann::json::array();
  for (const auto &message : request.messages) {
    nlohmann::json content = nlohmann::json::array();
    for (const auto &block : message.content) {
      std::visit(
          [&content](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Text>) {
              content.push_back({{"type", "text"}, {"text", value.text}});
            } else if constexpr (std::is_same_v<T, ToolUse>) {
              content.push_back({{"type", "tool_use"},
                                 {"id", value.id},
                                 {"name", value.name},
                                 {"input", value.input}});
            } else {
              nlohmann::json result{{"type", "tool_result"},
                                    {"tool_use_id", value.tool_use_id},
                                    {"content", value.content}};
              if (value.is_error)
                result["is_error"] = true;
              content.push_back(std::move(result));
            }
          },
          block);
    }
    messages.push_back({{"role", message.role}, {"content", content}});
  }

  nlohmann::json body{{"model", request.model},
                      {"max_tokens", request.max_tokens},
                      {"messages", messages}};
  if (!request.system.empty())
    body["system"] = request.system;
  if (!request.tools.empty()) {
    body["tools"] = nlohmann::json::array();
    for (const auto &tool : request.tools) {
      body["tools"].push_back({{"name", tool.name},
                               {"description", tool.description},
                               {"input_schema", tool.input_schema}});
    }
  }
  return body;
}

nlohmann::json openai_request_json(const Request &request) {
  nlohmann::json messages = nlohmann::json::array();
  if (!request.system.empty())
    messages.push_back({{"role", "system"}, {"content", request.system}});

  for (const auto &message : request.messages) {
    nlohmann::json tool_calls = nlohmann::json::array();
    std::string text;
    for (const auto &block : message.content) {
      std::visit(
          [&tool_calls, &text](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Text>) {
              if (!text.empty())
                text += '\n';
              text += value.text;
            } else if constexpr (std::is_same_v<T, ToolUse>) {
              tool_calls.push_back({{"id", value.id},
                                    {"type", "function"},
                                    {"function",
                                     {{"name", value.name},
                                      {"arguments", value.input.dump()}}}});
            }
          },
          block);
    }
    if (message.role == "assistant") {
      nlohmann::json assistant{{"role", "assistant"}};
      assistant["content"] =
          text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
      if (!tool_calls.empty())
        assistant["tool_calls"] = std::move(tool_calls);
      messages.push_back(std::move(assistant));
    } else {
      for (const auto &block : message.content) {
        if (const auto *result = std::get_if<ToolResult>(&block)) {
          messages.push_back({{"role", "tool"},
                              {"tool_call_id", result->tool_use_id},
                              {"content", result->content}});
        }
      }
      if (!text.empty() || message.content.empty())
        messages.push_back({{"role", message.role}, {"content", text}});
    }
  }

  nlohmann::json body{{"model", request.model},
                      {"max_tokens", request.max_tokens},
                      {"messages", messages}};
  if (!request.tools.empty()) {
    body["tools"] = nlohmann::json::array();
    for (const auto &tool : request.tools) {
      body["tools"].push_back({{"type", "function"},
                               {"function",
                                {{"name", tool.name},
                                 {"description", tool.description},
                                 {"parameters", tool.input_schema}}}});
    }
  }
  return body;
}

Response parse_anthropic_response(const nlohmann::json &body) {
  if (!body.contains("content") || !body["content"].is_array())
    throw nlohmann::json::type_error::create(302, "content must be an array",
                                             &body);
  Response response;
  response.stop_reason = body.value("stop_reason", "");
  for (const auto &block : body["content"]) {
    const std::string type = block.value("type", "");
    if (type == "text") {
      response.content.push_back({Text{block.value("text", "")}});
    } else if (type == "tool_use") {
      const std::string id = block.value("id", "");
      const std::string name = block.value("name", "");
      const nlohmann::json input =
          block.value("input", nlohmann::json::object());
      if (id.empty() || name.empty() || !input.is_object())
        throw nlohmann::json::type_error::create(
            302, "tool_use requires a nonempty id, name, and object input",
            &block);
      response.content.push_back({ToolUse{id, name, input}});
    }
  }
  return response;
}

Response parse_openai_response(const nlohmann::json &body) {
  if (!body.contains("choices") || !body["choices"].is_array() ||
      body["choices"].empty())
    throw nlohmann::json::type_error::create(
        302, "choices must be a nonempty array", &body);
  const auto &choice = body["choices"][0];
  if (!choice.contains("message") || !choice["message"].is_object())
    throw nlohmann::json::type_error::create(302, "choice message is missing",
                                             &choice);
  const auto &message = choice["message"];
  Response response;
  response.stop_reason = choice.value("finish_reason", "");
  if (message.contains("content") && message["content"].is_string()) {
    response.content.push_back({Text{message["content"].get<std::string>()}});
  }
  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto &call : message["tool_calls"]) {
      const auto &function = call.at("function");
      auto arguments = nlohmann::json::parse(function.value("arguments", "{}"));
      const std::string id = call.value("id", "");
      const std::string name = function.value("name", "");
      if (id.empty() || name.empty() || !arguments.is_object())
        throw nlohmann::json::type_error::create(
            302, "tool call requires a nonempty id, name, and object arguments",
            &call);
      response.content.push_back({ToolUse{id, name, std::move(arguments)}});
    }
  }
  return response;
}

} // namespace

Config Config::from_env(Provider provider) {
  Config config;
  config.provider = provider;
  if (provider == Provider::Anthropic) {
    config.api_key = environment_value("XOI_API_KEY");
    if (config.api_key.empty())
      config.api_key = environment_value("ANTHROPIC_API_KEY");
    config.base_url = environment_value("XOI_BASE_URL");
    if (config.base_url.empty())
      config.base_url = "https://api.anthropic.com";
  } else {
    config.api_key = environment_value("XOI_API_KEY");
    if (config.api_key.empty())
      config.api_key = environment_value("OPENAI_API_KEY");
    config.base_url = environment_value("XOI_BASE_URL");
    if (config.base_url.empty())
      config.base_url = "https://api.openai.com/v1";
  }
  return config;
}

Client::Client(Config config, std::unique_ptr<Transport> transport)
    : config_(std::move(config)),
      transport_(transport ? std::move(transport)
                           : std::make_unique<CurlTransport>()) {}

Result<Response> Client::send(const Request &request) const {
  if (config_.api_key.empty())
    return Error{ErrorKind::Configuration, "API key is empty", 0, false};
  if (config_.base_url.empty())
    return Error{ErrorKind::Configuration, "base URL is empty", 0, false};
  if (config_.timeout_sec <= 0 || config_.max_retries < 0 ||
      config_.max_retries > 10 || config_.initial_backoff_ms < 0 ||
      config_.max_backoff_sec < 0 || config_.max_response_bytes == 0)
    return Error{ErrorKind::Configuration, "invalid timeout or retry settings",
                 0, false};
  if (request.model.empty() || request.max_tokens <= 0 ||
      request.messages.empty())
    return Error{ErrorKind::Configuration,
                 "model, positive max_tokens, and messages are required", 0,
                 false};

  for (const auto &message : request.messages) {
    if (message.role != "user" && message.role != "assistant")
      return Error{ErrorKind::Configuration,
                   "message role must be user or assistant", 0, false};
  }
  for (const auto &tool : request.tools) {
    if (tool.name.empty() || !tool.input_schema.is_object())
      return Error{ErrorKind::Configuration,
                   "tool name and object input_schema are required", 0, false};
  }

  const auto body = config_.provider == Provider::Anthropic
                        ? anthropic_request_json(request)
                        : openai_request_json(request);
  std::vector<std::pair<std::string, std::string>> headers{
      {"Content-Type", "application/json"}};
  if (config_.provider == Provider::Anthropic) {
    headers.emplace_back("x-api-key", config_.api_key);
    headers.emplace_back("anthropic-version", "2023-06-01");
  } else {
    headers.emplace_back("Authorization", "Bearer " + config_.api_key);
  }

  const std::string url = endpoint(config_);
  const std::string serialized_body = body.dump();
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    const HttpResponse http =
        transport_->post(url, serialized_body, headers, config_.timeout_sec,
                         config_.max_response_bytes);
    if (http.response_too_large) {
      return Error{ErrorKind::ResponseTooLarge,
                   "response body exceeds configured size limit", 0, false};
    }
    if (http.response_callback_failed) {
      return Error{ErrorKind::Transport, "response body callback failed", 0,
                   false};
    }
    if (http.body.size() > config_.max_response_bytes) {
      return Error{ErrorKind::ResponseTooLarge,
                   "response body exceeds configured size limit", 0, false};
    }
    const bool transient_http =
        http.status == 429 || (http.status >= 500 && http.status <= 599);
    const bool transient_transport = !http.transport_error.empty();
    if ((transient_http || transient_transport) &&
        attempt < config_.max_retries) {
      const int64_t max_wait_ms =
          static_cast<int64_t>(config_.max_backoff_sec) * 1000;
      const long retry_after = retry_after_seconds(http);
      int64_t wait_ms = 0;
      if (retry_after >= 0) {
        wait_ms = std::min(static_cast<int64_t>(retry_after),
                           static_cast<int64_t>(config_.max_backoff_sec)) *
                  1000;
      } else {
        const int64_t exponential_ms =
            static_cast<int64_t>(config_.initial_backoff_ms) *
            static_cast<int64_t>(uint64_t{1} << static_cast<unsigned int>(
                                     std::min(attempt, 20)));
        wait_ms = std::min(exponential_ms, max_wait_ms);
      }
      if (wait_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      continue;
    }
    if (!http.transport_error.empty()) {
      return Error{ErrorKind::Transport, http.transport_error, 0,
                   transient_transport};
    }
    if (http.status < 200 || http.status >= 300) {
      return Error{ErrorKind::Http, error_message(http), http.status,
                   transient_http};
    }

    try {
      const auto parsed = nlohmann::json::parse(http.body);
      return config_.provider == Provider::Anthropic
                 ? Result<Response>(parse_anthropic_response(parsed))
                 : Result<Response>(parse_openai_response(parsed));
    } catch (const nlohmann::json::exception &error) {
      return protocol_error(std::string("invalid API response: ") +
                            error.what());
    }
  }
  return Error{ErrorKind::Transport, "request attempts exhausted", 0, true};
}

} // namespace xoi
