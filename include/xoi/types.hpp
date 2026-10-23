#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <variant>
#include <vector>

namespace xoi {

struct Text {
  std::string text;
};

struct ToolUse {
  std::string id;
  std::string name;
  nlohmann::json input = nlohmann::json::object();
};

struct ToolResult {
  std::string tool_use_id;
  std::string content;
  bool is_error = false;
};

using Block = std::variant<Text, ToolUse, ToolResult>;

struct Message {
  std::string role;
  std::vector<Block> content;
};

struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json input_schema = {{"type", "object"},
                                 {"properties", nlohmann::json::object()}};
};

enum class Provider {
  Anthropic,
  OpenAICompatible,
};

struct Request {
  std::string model;
  std::string system;
  std::vector<Message> messages;
  std::vector<ToolSpec> tools;
  int max_tokens = 1024;
};

struct Response {
  std::vector<Block> content;
  std::string stop_reason;
};

} // namespace xoi
