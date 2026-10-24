#include <xoi/client.hpp>

#include "environment.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
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
  if (config.provider == Provider::Ollama)
    return base + "/api/chat";
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

std::optional<std::uint64_t> usage_count(const nlohmann::json &usage,
                                         std::string_view name) {
  const auto value = usage.find(name);
  if (value == usage.end())
    return std::nullopt;
  if (value->is_number_unsigned())
    return value->get<std::uint64_t>();
  if (value->is_number_integer() && value->get<std::int64_t>() >= 0)
    return static_cast<std::uint64_t>(value->get<std::int64_t>());
  throw nlohmann::json::type_error::create(
      302,
      "usage count '" + std::string(name) + "' must be a non-negative integer",
      &usage);
}

Usage parse_named_usage(const nlohmann::json &values,
                        std::string_view input_name,
                        std::string_view output_name,
                        std::string_view cache_read_name = {},
                        std::string_view cache_creation_name = {}) {
  if (!values.is_object())
    throw nlohmann::json::type_error::create(302, "usage must be an object",
                                             &values);
  return Usage{
      usage_count(values, input_name), usage_count(values, output_name),
      cache_read_name.empty() ? std::nullopt
                              : usage_count(values, cache_read_name),
      cache_creation_name.empty() ? std::nullopt
                                  : usage_count(values, cache_creation_name)};
}

std::optional<Usage> parse_usage(const nlohmann::json &body,
                                 std::string_view input_name,
                                 std::string_view output_name,
                                 std::string_view cache_read_name = {},
                                 std::string_view cache_creation_name = {}) {
  const auto usage = body.find("usage");
  if (usage == body.end() || usage->is_null())
    return std::nullopt;
  if (!usage->is_object())
    throw nlohmann::json::type_error::create(302, "usage must be an object",
                                             &body);
  return parse_named_usage(*usage, input_name, output_name, cache_read_name,
                           cache_creation_name);
}

void merge_usage(std::optional<Usage> &target,
                 const std::optional<Usage> &update) {
  if (!update)
    return;
  if (!target)
    target = Usage{};
  if (update->input_tokens)
    target->input_tokens = update->input_tokens;
  if (update->output_tokens)
    target->output_tokens = update->output_tokens;
  if (update->cache_read_input_tokens)
    target->cache_read_input_tokens = update->cache_read_input_tokens;
  if (update->cache_creation_input_tokens)
    target->cache_creation_input_tokens = update->cache_creation_input_tokens;
}

bool valid_header_name(const std::string &name) {
  if (name.empty())
    return false;
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  return std::all_of(name.begin(), name.end(), [&punctuation](unsigned char c) {
    return c < 128 &&
           (std::isalnum(c) != 0 ||
            punctuation.find(static_cast<char>(c)) != std::string_view::npos);
  });
}

bool contains_line_break(const std::string &value) {
  return value.find_first_of("\r\n") != std::string::npos;
}

bool equal_ascii_case_insensitive(const std::string &left,
                                  const std::string &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
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
  if (request.temperature)
    body["temperature"] = *request.temperature;
  if (request.top_p)
    body["top_p"] = *request.top_p;
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

nlohmann::json
openai_request_json(const Request &request, bool streaming,
                    OpenAITokenLimitParameter token_limit_parameter,
                    bool include_stream_usage) {
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

  nlohmann::json body{{"model", request.model}, {"messages", messages}};
  body[token_limit_parameter == OpenAITokenLimitParameter::MaxTokens
           ? "max_tokens"
           : "max_completion_tokens"] = request.max_tokens;
  if (request.temperature)
    body["temperature"] = *request.temperature;
  if (request.top_p)
    body["top_p"] = *request.top_p;
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
  if (streaming && include_stream_usage)
    body["stream_options"] = {{"include_usage", true}};
  return body;
}

nlohmann::json ollama_request_json(const Request &request, bool streaming,
                                   bool send_num_predict) {
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
              tool_calls.push_back(
                  {{"function",
                    {{"name", value.name}, {"arguments", value.input}}}});
            }
          },
          block);
    }
    if (message.role == "assistant") {
      nlohmann::json assistant{{"role", "assistant"}};
      assistant["content"] = text;
      if (!tool_calls.empty())
        assistant["tool_calls"] = std::move(tool_calls);
      messages.push_back(std::move(assistant));
    } else {
      for (const auto &block : message.content) {
        if (const auto *result = std::get_if<ToolResult>(&block))
          messages.push_back({{"role", "tool"}, {"content", result->content}});
      }
      if (!text.empty() || message.content.empty())
        messages.push_back({{"role", message.role}, {"content", text}});
    }
  }

  nlohmann::json options = nlohmann::json::object();
  if (send_num_predict)
    options["num_predict"] = request.max_tokens;
  if (request.temperature)
    options["temperature"] = *request.temperature;
  if (request.top_p)
    options["top_p"] = *request.top_p;
  nlohmann::json body{{"model", request.model}, {"messages", messages}};
  if (!options.empty())
    body["options"] = std::move(options);
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
  body["stream"] = streaming;
  return body;
}

Response parse_anthropic_response(const nlohmann::json &body) {
  if (!body.contains("content") || !body["content"].is_array())
    throw nlohmann::json::type_error::create(302, "content must be an array",
                                             &body);
  Response response;
  response.stop_reason = body.value("stop_reason", "");
  response.usage =
      parse_usage(body, "input_tokens", "output_tokens",
                  "cache_read_input_tokens", "cache_creation_input_tokens");
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
  response.usage = parse_usage(body, "prompt_tokens", "completion_tokens");
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

Response parse_ollama_response(const nlohmann::json &body) {
  if (!body.contains("message") || !body["message"].is_object())
    throw nlohmann::json::type_error::create(302, "message is missing", &body);
  const auto &message = body["message"];
  Response response;
  response.stop_reason = body.value("done_reason", "");
  if (body.contains("prompt_eval_count") || body.contains("eval_count"))
    response.usage = parse_named_usage(body, "prompt_eval_count", "eval_count");
  if (message.contains("content") && message["content"].is_string()) {
    response.content.push_back({Text{message["content"].get<std::string>()}});
  }
  if (message.contains("tool_calls")) {
    if (!message["tool_calls"].is_array())
      throw nlohmann::json::type_error::create(
          302, "tool_calls must be an array", &message);
    std::size_t index = 0;
    for (const auto &call : message["tool_calls"]) {
      if (!call.contains("function") || !call["function"].is_object())
        throw nlohmann::json::type_error::create(
            302, "tool call function is missing", &call);
      const auto &function = call["function"];
      const std::string name = function.value("name", "");
      const nlohmann::json input =
          function.value("arguments", nlohmann::json::object());
      if (name.empty() || !input.is_object())
        throw nlohmann::json::type_error::create(
            302, "tool call requires a nonempty name and object arguments",
            &call);
      response.content.push_back(
          {ToolUse{"ollama-tool-" + std::to_string(index++), name, input}});
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
    result.usage = usage_;
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
      if (event.contains("usage") && !event["usage"].is_null())
        merge_usage(usage_, parse_named_usage(event["usage"], "input_tokens",
                                              "output_tokens",
                                              "cache_read_input_tokens",
                                              "cache_creation_input_tokens"));
      const auto reason = event["delta"].find("stop_reason");
      if (reason != event["delta"].end() && reason->is_string())
        stop_reason_ = reason->get<std::string>();
    } else if (type == "message_start") {
      if (event.contains("message") && event["message"].is_object()) {
        if (event["message"].contains("usage") &&
            !event["message"]["usage"].is_null())
          merge_usage(usage_, parse_named_usage(event["message"]["usage"],
                                                "input_tokens", "output_tokens",
                                                "cache_read_input_tokens",
                                                "cache_creation_input_tokens"));
        const auto reason = event["message"].find("stop_reason");
        if (reason != event["message"].end() && reason->is_string())
          stop_reason_ = reason->get<std::string>();
      }
    }
  }

  void process_openai_event(const nlohmann::json &event) {
    if (event.contains("usage") && !event["usage"].is_null())
      usage_ = parse_named_usage(event["usage"], "prompt_tokens",
                                 "completion_tokens");
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
  std::optional<Usage> usage_;
  std::exception_ptr callback_exception_;
  bool saw_event_ = false;
  std::map<std::size_t, StreamBlock> blocks_;
};

class OllamaStreamDecoder {
public:
  explicit OllamaStreamDecoder(
      const std::function<void(std::string_view)> &on_text_delta)
      : on_text_delta_(on_text_delta) {}

  void feed(std::string_view chunk) {
    if (!error_.empty())
      return;
    for (const char character : chunk) {
      if (character == '\n') {
        process_line();
        line_.clear();
      } else {
        line_.push_back(character);
      }
      if (!error_.empty())
        return;
    }
  }

  void finish() {
    if (!line_.empty()) {
      process_line();
      line_.clear();
    }
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
    Response response;
    response.stop_reason = stop_reason_;
    response.usage = usage_;
    if (!text_.empty())
      response.content.push_back({Text{text_}});
    for (std::size_t index = 0; index < tools_.size(); ++index) {
      const auto &tool = tools_[index];
      if (tool.name.empty() || !tool.input.is_object())
        return protocol_error(
            "streamed tool call requires a nonempty name and object arguments");
      response.content.push_back({ToolUse{
          "ollama-tool-" + std::to_string(index), tool.name, tool.input}});
    }
    return response;
  }

private:
  struct StreamTool {
    std::string name;
    nlohmann::json input = nlohmann::json::object();
  };

  void process_line() {
    if (!line_.empty() && line_.back() == '\r')
      line_.pop_back();
    if (line_.empty())
      return;
    nlohmann::json event;
    try {
      event = nlohmann::json::parse(line_);
    } catch (const nlohmann::json::exception &error) {
      error_ = std::string("invalid Ollama streaming event: ") + error.what();
      return;
    }
    if (!event.is_object()) {
      error_ = "Ollama streaming event must be an object";
      return;
    }
    saw_event_ = true;
    if (event.contains("error")) {
      error_ = "Ollama streaming error: " +
               (event["error"].is_string() ? event["error"].get<std::string>()
                                           : event["error"].dump());
      return;
    }
    if (event.contains("done_reason") && event["done_reason"].is_string())
      stop_reason_ = event["done_reason"].get<std::string>();
    if (event.value("done", false) &&
        (event.contains("prompt_eval_count") || event.contains("eval_count")))
      usage_ = parse_named_usage(event, "prompt_eval_count", "eval_count");
    if (!event.contains("message"))
      return;
    if (!event["message"].is_object()) {
      error_ = "Ollama streaming message must be an object";
      return;
    }
    const auto &message = event["message"];
    if (message.contains("content")) {
      if (!message["content"].is_string()) {
        error_ = "Ollama streaming content must be a string";
        return;
      }
      const auto &text = message["content"].get_ref<const std::string &>();
      text_.append(text);
      if (!text.empty()) {
        try {
          on_text_delta_(text);
        } catch (...) {
          callback_exception_ = std::current_exception();
          throw;
        }
      }
    }
    if (!message.contains("tool_calls"))
      return;
    if (!message["tool_calls"].is_array()) {
      error_ = "Ollama streaming tool_calls must be an array";
      return;
    }
    for (const auto &call : message["tool_calls"]) {
      if (!call.contains("function") || !call["function"].is_object()) {
        error_ = "Ollama streaming tool call function is missing";
        return;
      }
      const auto &function = call["function"];
      const std::string name = function.value("name", "");
      const nlohmann::json input =
          function.value("arguments", nlohmann::json::object());
      if (name.empty() || !input.is_object()) {
        error_ =
            "Ollama streaming tool call requires a name and object arguments";
        return;
      }
      tools_.push_back({name, input});
    }
  }

  const std::function<void(std::string_view)> &on_text_delta_;
  std::string line_;
  std::string text_;
  std::string stop_reason_;
  std::string error_;
  std::optional<Usage> usage_;
  std::exception_ptr callback_exception_;
  bool saw_event_ = false;
  std::vector<StreamTool> tools_;
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
  } else if (provider == Provider::Ollama) {
    config.api_key = detail::read_environment_variable("XOI_API_KEY");
    config.base_url = detail::read_environment_variable("XOI_BASE_URL");
    if (config.base_url.empty())
      config.base_url = "http://localhost:11434";
  } else {
    config.api_key = detail::read_environment_variable("XOI_API_KEY");
    if (config.api_key.empty())
      config.api_key = detail::read_environment_variable("OPENAI_API_KEY");
    config.base_url = detail::read_environment_variable("XOI_BASE_URL");
    if (config.base_url.empty())
      config.base_url = "https://api.openai.com/v1";
  }
  config.api_key_header =
      detail::read_environment_variable("XOI_API_KEY_HEADER");
  config.proxy_url =
      detail::read_optional_environment_variable("XOI_PROXY_URL");
  if (detail::read_environment_variable("XOI_OPENAI_TOKEN_LIMIT_PARAMETER") ==
      "max_completion_tokens") {
    config.openai_token_limit_parameter =
        OpenAITokenLimitParameter::MaxCompletionTokens;
  }
  return config;
}

Client::Client(Config config, std::unique_ptr<Transport> transport)
    : config_(std::move(config)),
      transport_(transport
                     ? std::move(transport)
                     : std::make_unique<CurlTransport>(config_.proxy_url)) {}

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
  if (request.stop_token.stop_requested())
    return Error{ErrorKind::Cancelled, "request cancelled", 0, false};
  if (config_.provider != Provider::Ollama && config_.api_key.empty())
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
  if ((request.temperature && !std::isfinite(*request.temperature)) ||
      (request.top_p && !std::isfinite(*request.top_p)))
    return Error{ErrorKind::Configuration,
                 "temperature and top_p must be finite numbers", 0, false};
  if (!config_.api_key_header.empty() &&
      (!valid_header_name(config_.api_key_header) ||
       equal_ascii_case_insensitive(config_.api_key_header, "Content-Type") ||
       equal_ascii_case_insensitive(config_.api_key_header,
                                    "Anthropic-Version")))
    return Error{ErrorKind::Configuration, "invalid API key header name", 0,
                 false};
  if (!config_.api_key_header.empty() && config_.api_key.empty())
    return Error{ErrorKind::Configuration,
                 "custom API key header requires an API key", 0, false};
  if (config_.provider == Provider::OpenAICompatible &&
      config_.openai_token_limit_parameter !=
          OpenAITokenLimitParameter::MaxTokens &&
      config_.openai_token_limit_parameter !=
          OpenAITokenLimitParameter::MaxCompletionTokens)
    return Error{ErrorKind::Configuration,
                 "invalid OpenAI-compatible token limit parameter", 0, false};
  if (contains_line_break(config_.api_key))
    return Error{ErrorKind::Configuration,
                 "API key must not contain a line break", 0, false};

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
      : config_.provider == Provider::Ollama
          ? ollama_request_json(request, on_text_delta != nullptr,
                                config_.ollama_send_num_predict)
          : openai_request_json(request, on_text_delta != nullptr,
                                config_.openai_token_limit_parameter,
                                config_.openai_stream_include_usage);
  std::vector<std::pair<std::string, std::string>> headers{
      {"Content-Type", "application/json"}};
  if (on_text_delta)
    headers.emplace_back("Accept", config_.provider == Provider::Ollama
                                       ? "application/x-ndjson"
                                       : "text/event-stream");
  if (!config_.api_key.empty()) {
    if (!config_.api_key_header.empty()) {
      headers.emplace_back(config_.api_key_header, config_.api_key);
    } else if (config_.provider == Provider::Anthropic) {
      headers.emplace_back("x-api-key", config_.api_key);
    } else {
      headers.emplace_back("Authorization", "Bearer " + config_.api_key);
    }
  }
  if (config_.provider == Provider::Anthropic) {
    headers.emplace_back("anthropic-version", "2023-06-01");
  }

  const std::string url = endpoint(config_);
  const std::string serialized_body = body.dump();
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    if (request.stop_token.stop_requested())
      return Error{ErrorKind::Cancelled, "request cancelled", 0, false};
    std::unique_ptr<SseDecoder> sse_decoder;
    std::unique_ptr<OllamaStreamDecoder> ollama_decoder;
    std::exception_ptr callback_exception;
    HttpResponse http;
    if (on_text_delta) {
      if (config_.provider == Provider::Ollama) {
        ollama_decoder = std::make_unique<OllamaStreamDecoder>(*on_text_delta);
      } else {
        sse_decoder =
            std::make_unique<SseDecoder>(config_.provider, *on_text_delta);
      }
      const StreamChunkHandler on_chunk = [&](std::string_view chunk) {
        try {
          if (ollama_decoder)
            ollama_decoder->feed(chunk);
          else
            sse_decoder->feed(chunk);
        } catch (...) {
          callback_exception = ollama_decoder
                                   ? ollama_decoder->callback_exception()
                                   : sse_decoder->callback_exception();
          throw;
        }
      };
      http = transport_->post_stream(
          url, serialized_body, headers, config_.timeout_sec,
          config_.max_response_bytes, on_chunk, request.stop_token);
    } else {
      http =
          transport_->post(url, serialized_body, headers, config_.timeout_sec,
                           config_.max_response_bytes, request.stop_token);
    }
    if (http.cancelled || request.stop_token.stop_requested())
      return Error{ErrorKind::Cancelled, "request cancelled", 0, false};
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
      std::condition_variable_any backoff_condition;
      std::mutex backoff_mutex;
      std::unique_lock lock(backoff_mutex);
      backoff_condition.wait_for(lock, request.stop_token,
                                 std::chrono::milliseconds(wait_ms),
                                 [] { return false; });
      if (request.stop_token.stop_requested())
        return Error{ErrorKind::Cancelled, "request cancelled", 0, false};
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
        if (ollama_decoder)
          ollama_decoder->finish();
        else
          sse_decoder->finish();
      } catch (const std::exception &error) {
        return Error{ErrorKind::Callback,
                     std::string("stream callback failed: ") + error.what(), 0,
                     false, TransportFailure::Callback};
      } catch (...) {
        return Error{ErrorKind::Callback, "stream callback failed", 0, false,
                     TransportFailure::Callback};
      }
      const std::string &stream_error =
          ollama_decoder ? ollama_decoder->error() : sse_decoder->error();
      if (!stream_error.empty())
        return protocol_error(stream_error);
      return ollama_decoder ? ollama_decoder->response()
                            : sse_decoder->response();
    }

    try {
      const auto parsed = nlohmann::json::parse(http.body);
      if (config_.provider == Provider::Anthropic)
        return parse_anthropic_response(parsed);
      if (config_.provider == Provider::Ollama)
        return parse_ollama_response(parsed);
      return parse_openai_response(parsed);
    } catch (const nlohmann::json::exception &error) {
      return protocol_error(std::string("invalid API response: ") +
                            error.what());
    }
  }
  return Error{ErrorKind::Transport, "request attempts exhausted", 0, true};
}

} // namespace xoi
