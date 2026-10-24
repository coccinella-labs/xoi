#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace xoi {

/// A plain text content block.
struct Text {
  std::string text;
};

/// A model request to execute a named tool with JSON input.
struct ToolUse {
  std::string id;
  std::string name;
  nlohmann::json input = nlohmann::json::object();
};

/// The result of executing a tool requested by the model.
struct ToolResult {
  std::string tool_use_id;
  std::string content;
  bool is_error = false;
};

/// A supported message content block.
using Block = std::variant<Text, ToolUse, ToolResult>;

/// A user or assistant message containing ordered content blocks.
struct Message {
  std::string role;
  std::vector<Block> content;
};

/// A tool description and its JSON Schema input shape.
struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json input_schema = {{"type", "object"},
                                 {"properties", nlohmann::json::object()}};
};

/// The API wire format selected for a client.
enum class Provider : std::uint8_t {
  Anthropic,
  OpenAICompatible,
  Ollama,
};

/// A request sent to an LLM provider.
struct Request {
  std::string model;
  std::string system;
  std::vector<Message> messages;
  std::vector<ToolSpec> tools;
  int max_tokens = 1024;
  std::optional<double> temperature;
  std::optional<double> top_p;
  /// Optional cancellation signal for the provider request.
  std::stop_token stop_token;
};

/// Provider-reported token counts. Missing counts remain unknown; no costs
/// are calculated from usage.
struct Usage {
  std::optional<std::uint64_t> input_tokens;
  std::optional<std::uint64_t> output_tokens;
  std::optional<std::uint64_t> cache_read_input_tokens;
  std::optional<std::uint64_t> cache_creation_input_tokens;
};

/// A parsed response from an LLM provider.
struct Response {
  std::vector<Block> content;
  std::string stop_reason;
  /// Token counts reported by the provider, when available.
  std::optional<Usage> usage;
};

} // namespace xoi
