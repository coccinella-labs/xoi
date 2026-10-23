#include <xoi/client.hpp>

#include "environment.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <map>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace xoi {
namespace {

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
    return (std::max)(seconds, 0L);

  const time_t retry_at = curl_getdate(value.c_str(), nullptr);
  if (retry_at < 0)
    return -1;
  return (std::max)(static_cast<long>(retry_at - std::time(nullptr)), 0L);
}

Error protocol_error(std::string message) {
  return {ErrorKind::Protocol, std::move(message), 0, false};
}

nlohmann::json anthropic_request_json(const Request &request, bool streaming) {
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
  if (streaming)
    body["stream"] = true;
  return body;
}

nlohmann::json openai_request_json(const Request &request, bool streaming) {
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
  if (streaming)
    body["stream"] = true;
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

struct StreamTool {
  std::string id;
  std::string name;
  std::string arguments;
  bool arguments_started = false;
};

using StreamBlock = std::variant<std::string, StreamTool>;

class SseDecoder {
public:
  SseDecoder(Provider provider,
             const std::function<void(std::string_view)> &on_text_delta)
      : provider_(provider), on_text_delta_(on_text_delta) {}

  void feed(std::string_view chunk) {
    if (!error_.empty())
      return;
    for (const char character : chunk) {
      if (!error_.empty())
        return;
      if (character == '\n') {
        process_line();
        line_.clear();
      } else {
        line_.push_back(character);
      }
    }
  }

  void finish() {
    if (!line_.empty()) {
      process_line();
      line_.clear();
    }
    dispatch_event();
  }

  const std::string &error() const {
    return error_;
  }

  const std::exception_ptr &callback_exception() const {
    return callback_exception_;
  }

  Result<Response> response() const {
    if (!saw_event_)
      return protocol_error("stream response contained no events");
    Response result;
    result.stop_reason = stop_reason_;
    for (const auto &[index, block] : blocks_) {
      (void)index;
      if (const auto *text = std::get_if<std::string>(&block)) {
        if (!text->empty())
          result.content.push_back({Text{*text}});
        continue;
      }
      const auto &tool = std::get<StreamTool>(block);
      nlohmann::json input;
      try {
        input = nlohmann::json::parse(tool.arguments.empty()
                                          ? std::string_view("{}")
                                          : std::string_view(tool.arguments));
      } catch (const nlohmann::json::exception &error) {
        return protocol_error(std::string("invalid streamed tool arguments: ") +
                              error.what());
      }
      if (tool.id.empty() || tool.name.empty() || !input.is_object())
        return protocol_error(
            "streamed tool call requires an id, name, and object arguments");
      result.content.push_back({ToolUse{tool.id, tool.name, std::move(input)}});
    }
    return result;
  }

private:
  void process_line() {
    if (!line_.empty() && line_.back() == '\r')
      line_.pop_back();
    if (line_.empty()) {
      dispatch_event();
      return;
    }
    if (line_.front() == ':')
      return;
    const auto separator = line_.find(':');
    const std::string_view field(line_.data(), separator == std::string::npos
                                                   ? line_.size()
                                                   : separator);
    std::string_view value =
        separator == std::string::npos
            ? std::string_view{}
            : std::string_view(line_).substr(separator + 1);
    if (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    if (field == "data") {
      if (!data_.empty())
        data_.push_back('\n');
      data_.append(value);
    } else if (field == "event") {
      event_ = value;
    }
  }

  void dispatch_event() {
    if (data_.empty())
      return;
    if (data_ == "[DONE]") {
      data_.clear();
      event_.clear();
      return;
    }
    nlohmann::json event;
    try {
      event = nlohmann::json::parse(data_);
    } catch (const nlohmann::json::exception &error) {
      error_ = std::string("invalid streaming event: ") + error.what();
      data_.clear();
      event_.clear();
      return;
    }
    data_.clear();
    saw_event_ = true;
    if (event.contains("error")) {
      const auto &provider_error = event["error"];
      const std::string message =
          provider_error.is_object() && provider_error.contains("message") &&
                  provider_error["message"].is_string()
              ? provider_error["message"].get<std::string>()
              : provider_error.dump();
      error_ = "provider streaming error: " + message;
      event_.clear();
      return;
    }
    try {
      if (provider_ == Provider::Anthropic)
        process_anthropic_event(event);
      else
        process_openai_event(event);
    } catch (const nlohmann::json::exception &error) {
      if (callback_exception_)
        throw;
      error_ = std::string("invalid provider streaming event: ") + error.what();
    }
    event_.clear();
  }

  void append_text(std::size_t index, const std::string &text) {
    auto [it, inserted] = blocks_.try_emplace(index, std::string{});
    if (!std::holds_alternative<std::string>(it->second)) {
      error_ = "stream contains conflicting content block types";
      return;
    }
    std::get<std::string>(it->second).append(text);
    if (!text.empty()) {
      try {
        on_text_delta_(text);
      } catch (...) {
        callback_exception_ = std::current_exception();
        throw;
      }
    }
    (void)inserted;
  }

  StreamTool &tool_at(std::size_t index) {
    auto [it, inserted] = blocks_.try_emplace(index, StreamTool{});
    if (!std::holds_alternative<StreamTool>(it->second)) {
      error_ = "stream contains conflicting content block types";
      it->second = StreamTool{};
    }
    (void)inserted;
    return std::get<StreamTool>(it->second);
  }

  void process_anthropic_event(const nlohmann::json &event) {
    const std::string type = event.value("type", event_);
    if (type == "content_block_start") {
      const auto index = event.value("index", std::size_t{0});
      if (!event.contains("content_block") ||
          !event["content_block"].is_object())
        return;
      const auto &block = event["content_block"];
      const std::string block_type = block.value("type", "");
      if (block_type == "text") {
        blocks_.try_emplace(index, std::string{});
      } else if (block_type == "tool_use") {
        auto &tool = tool_at(index);
        tool.id = block.value("id", "");
        tool.name = block.value("name", "");
        const auto input = block.value("input", nlohmann::json::object());
        if (!input.is_object()) {
          error_ = "streamed tool input must be an object";
          return;
        }
        if (!input.empty())
          tool.arguments = input.dump();
      }
    } else if (type == "content_block_delta") {
      const auto index = event.value("index", std::size_t{0});
      if (!event.contains("delta") || !event["delta"].is_object())
        return;
      const auto &delta = event["delta"];
      const std::string delta_type = delta.value("type", "");
      if (delta_type == "text_delta" && delta.contains("text") &&
          delta["text"].is_string()) {
        append_text(index, delta["text"].get<std::string>());
      } else if (delta_type == "input_json_delta" &&
                 delta.contains("partial_json") &&
                 delta["partial_json"].is_string()) {
        auto &tool = tool_at(index);
        if (!tool.arguments_started) {
          tool.arguments.clear();
          tool.arguments_started = true;
        }
        tool.arguments += delta["partial_json"].get<std::string>();
      }
    } else if (type == "message_delta" && event.contains("delta") &&
               event["delta"].is_object()) {
      const auto &reason = event["delta"]["stop_reason"];
      if (reason.is_string())
        stop_reason_ = reason.get<std::string>();
    } else if (type == "message_start") {
      if (event.contains("message") && event["message"].is_object()) {
        const auto &reason = event["message"]["stop_reason"];
        if (reason.is_string())
          stop_reason_ = reason.get<std::string>();
      }
    }
  }

  void process_openai_event(const nlohmann::json &event) {
    if (!event.contains("choices"))
      return;
    if (!event["choices"].is_array()) {
      error_ = "streaming event choices must be an array";
      return;
    }
    if (event["choices"].empty())
      return;
    const auto &choice = event["choices"][0];
    if (!choice.is_object()) {
      error_ = "streaming event choice must be an object";
      return;
    }
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null())
      stop_reason_ = choice["finish_reason"].get<std::string>();
    if (!choice.contains("delta"))
      return;
    if (!choice["delta"].is_object()) {
      error_ = "streaming event delta must be an object";
      return;
    }
    const auto &delta = choice["delta"];
    if (delta.contains("content") && delta["content"].is_string())
      append_text(0, delta["content"].get<std::string>());
    if (!delta.contains("tool_calls"))
      return;
    if (!delta["tool_calls"].is_array()) {
      error_ = "streaming event tool_calls must be an array";
      return;
    }
    for (const auto &call : delta["tool_calls"]) {
      const auto tool_index = call.value("index", std::size_t{0});
      auto &tool = tool_at(tool_index + 1);
      if (call.contains("id") && call["id"].is_string())
        tool.id += call["id"].get<std::string>();
      if (!call.contains("function") || !call["function"].is_object())
        continue;
      const auto &function = call["function"];
      if (function.contains("name") && function["name"].is_string())
        tool.name += function["name"].get<std::string>();
      if (function.contains("arguments") && function["arguments"].is_string()) {
        tool.arguments_started = true;
        tool.arguments += function["arguments"].get<std::string>();
      }
    }
  }

  Provider provider_;
  const std::function<void(std::string_view)> &on_text_delta_;
  std::string line_;
  std::string data_;
  std::string event_;
  std::string stop_reason_;
  std::string error_;
  std::exception_ptr callback_exception_;
  bool saw_event_ = false;
  std::map<std::size_t, StreamBlock> blocks_;
};

} // namespace

Config Config::from_env(Provider provider) {
  Config config;
  config.provider = provider;
  if (provider == Provider::Anthropic) {
    config.api_key = detail::read_environment_variable("XOI_API_KEY");
    if (config.api_key.empty())
      config.api_key = detail::read_environment_variable("ANTHROPIC_API_KEY");
    config.base_url = detail::read_environment_variable("XOI_BASE_URL");
    if (config.base_url.empty())
      config.base_url = "https://api.anthropic.com";
  } else {
    config.api_key = detail::read_environment_variable("XOI_API_KEY");
    if (config.api_key.empty())
      config.api_key = detail::read_environment_variable("OPENAI_API_KEY");
    config.base_url = detail::read_environment_variable("XOI_BASE_URL");
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
  return send_impl(request, nullptr);
}

Result<Response> Client::send_stream(
    const Request &request,
    const std::function<void(std::string_view)> &on_text_delta) const {
  if (!on_text_delta)
    return Error{ErrorKind::Configuration, "stream text callback is required",
                 0, false};
  return send_impl(request, &on_text_delta);
}

Result<Response> Client::send_impl(
    const Request &request,
    const std::function<void(std::string_view)> *on_text_delta) const {
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

  const auto body =
      config_.provider == Provider::Anthropic
          ? anthropic_request_json(request, on_text_delta != nullptr)
          : openai_request_json(request, on_text_delta != nullptr);
  std::vector<std::pair<std::string, std::string>> headers{
      {"Content-Type", "application/json"}};
  if (on_text_delta)
    headers.emplace_back("Accept", "text/event-stream");
  if (config_.provider == Provider::Anthropic) {
    headers.emplace_back("x-api-key", config_.api_key);
    headers.emplace_back("anthropic-version", "2023-06-01");
  } else {
    headers.emplace_back("Authorization", "Bearer " + config_.api_key);
  }

  const std::string url = endpoint(config_);
  const std::string serialized_body = body.dump();
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    std::unique_ptr<SseDecoder> decoder;
    std::exception_ptr callback_exception;
    HttpResponse http;
    if (on_text_delta) {
      decoder = std::make_unique<SseDecoder>(config_.provider, *on_text_delta);
      const StreamChunkHandler on_chunk = [&](std::string_view chunk) {
        try {
          decoder->feed(chunk);
        } catch (...) {
          callback_exception = decoder->callback_exception();
          throw;
        }
      };
      http = transport_->post_stream(url, serialized_body, headers,
                                     config_.timeout_sec,
                                     config_.max_response_bytes, on_chunk);
    } else {
      http = transport_->post(url, serialized_body, headers,
                              config_.timeout_sec, config_.max_response_bytes);
    }
    if (http.response_too_large) {
      return Error{ErrorKind::ResponseTooLarge,
                   "response body exceeds configured size limit", 0, false};
    }
    if (http.response_callback_failed) {
      if (callback_exception) {
        try {
          std::rethrow_exception(callback_exception);
        } catch (const std::exception &error) {
          return Error{ErrorKind::Callback,
                       std::string("stream callback failed: ") + error.what(),
                       0, false, TransportFailure::Callback};
        } catch (...) {
          return Error{ErrorKind::Callback, "stream callback failed", 0, false,
                       TransportFailure::Callback};
        }
      }
      return Error{ErrorKind::Transport, "response body callback failed", 0,
                   false};
    }
    if (http.body.size() > config_.max_response_bytes) {
      return Error{ErrorKind::ResponseTooLarge,
                   "response body exceeds configured size limit", 0, false};
    }
    const bool transient_http =
        http.status == 429 || (http.status >= 500 && http.status <= 599);
    const bool retryable_transport_failure =
        http.transport_failure == TransportFailure::Connection ||
        http.transport_failure == TransportFailure::Timeout;
    const bool transient_transport =
        !http.transport_error.empty() && retryable_transport_failure &&
        http.response_bytes_received == 0 && http.body.empty();
    if ((transient_http || transient_transport) &&
        attempt < config_.max_retries) {
      const int64_t max_wait_ms =
          static_cast<int64_t>(config_.max_backoff_sec) * 1000;
      const long retry_after = retry_after_seconds(http);
      int64_t wait_ms = 0;
      if (retry_after >= 0) {
        wait_ms = (std::min)(static_cast<int64_t>(retry_after),
                             static_cast<int64_t>(config_.max_backoff_sec)) *
                  1000;
      } else {
        const int64_t exponential_ms =
            static_cast<int64_t>(config_.initial_backoff_ms) *
            static_cast<int64_t>(uint64_t{1} << static_cast<unsigned int>(
                                     (std::min)(attempt, 20)));
        wait_ms = (std::min)(exponential_ms, max_wait_ms);
      }
      if (wait_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      continue;
    }
    if (!http.transport_error.empty()) {
      return Error{ErrorKind::Transport, http.transport_error, 0,
                   transient_transport, http.transport_failure};
    }
    if (http.status < 200 || http.status >= 300) {
      return Error{ErrorKind::Http, error_message(http), http.status,
                   transient_http};
    }

    if (on_text_delta) {
      try {
        decoder->finish();
      } catch (const std::exception &error) {
        return Error{ErrorKind::Callback,
                     std::string("stream callback failed: ") + error.what(), 0,
                     false, TransportFailure::Callback};
      } catch (...) {
        return Error{ErrorKind::Callback, "stream callback failed", 0, false,
                     TransportFailure::Callback};
      }
      if (!decoder->error().empty())
        return protocol_error(decoder->error());
      return decoder->response();
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
