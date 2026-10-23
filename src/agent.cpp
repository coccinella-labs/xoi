#include <xoi/agent.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace xoi {
namespace {

bool supported_type(std::string_view type) {
  constexpr std::array<std::string_view, 7> types = {
      "string", "integer", "number", "boolean", "object", "array", "null"};
  return std::find(types.begin(), types.end(), type) != types.end();
}

bool matches_type(std::string_view type, const nlohmann::json &value) {
  if (type == "string")
    return value.is_string();
  if (type == "integer")
    return value.is_number_integer();
  if (type == "number")
    return value.is_number();
  if (type == "boolean")
    return value.is_boolean();
  if (type == "object")
    return value.is_object();
  if (type == "array")
    return value.is_array();
  if (type == "null")
    return value.is_null();
  return false;
}

bool supported_schema_keyword(std::string_view keyword) {
  constexpr std::array<std::string_view, 18> keywords = {
      "type",        "properties", "required",
      "items",       "enum",       "additionalProperties",
      "minimum",     "maximum",    "minLength",
      "maxLength",   "minItems",   "maxItems",
      "description", "title",      "default",
      "examples",    "$schema",    "$comment"};
  return std::find(keywords.begin(), keywords.end(), keyword) != keywords.end();
}

bool is_finite_number(const nlohmann::json &value) {
  return value.is_number() &&
         (!value.is_number_float() || std::isfinite(value.get<double>()));
}

bool is_nonnegative_integer(const nlohmann::json &value) {
  return value.is_number_unsigned() ||
         (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

std::string validate_schema_definition(const nlohmann::json &schema,
                                       const std::string &path) {
  if (!schema.is_object())
    return path + " must be an object";

  for (auto it = schema.begin(); it != schema.end(); ++it) {
    if (!supported_schema_keyword(it.key()))
      return path + " uses unsupported keyword '" + it.key() + "'";
  }
  if (schema.contains("type") &&
      (!schema["type"].is_string() ||
       !supported_type(schema["type"].get<std::string>())))
    return path + " type must be a supported JSON type";
  if (schema.contains("properties")) {
    if (!schema["properties"].is_object())
      return path + " properties must be an object";
    for (auto it = schema["properties"].begin();
         it != schema["properties"].end(); ++it) {
      std::string error = validate_schema_definition(
          it.value(), path + ".properties." + it.key());
      if (!error.empty())
        return error;
    }
  }
  if (schema.contains("required")) {
    if (!schema["required"].is_array())
      return path + " required must be an array";
    for (const auto &required : schema["required"]) {
      if (!required.is_string())
        return path + " required entries must be strings";
    }
  }
  if (schema.contains("items")) {
    std::string error =
        validate_schema_definition(schema["items"], path + ".items");
    if (!error.empty())
      return error;
  }
  if (schema.contains("enum") &&
      (!schema["enum"].is_array() || schema["enum"].empty()))
    return path + " enum must be a non-empty array";
  if (schema.contains("additionalProperties") &&
      !schema["additionalProperties"].is_boolean())
    return path + " additionalProperties must be a boolean";
  for (const std::string_view bound : {"minimum", "maximum"}) {
    if (schema.contains(bound) && !is_finite_number(schema[bound]))
      return path + " " + std::string(bound) + " must be a finite number";
  }
  for (const std::string_view bound :
       {"minLength", "maxLength", "minItems", "maxItems"}) {
    if (schema.contains(bound) && !is_nonnegative_integer(schema[bound]))
      return path + " " + std::string(bound) +
             " must be a non-negative integer";
  }
  for (const std::string_view annotation :
       {"description", "title", "$schema", "$comment"}) {
    if (schema.contains(annotation) && !schema[annotation].is_string())
      return path + " " + std::string(annotation) + " must be a string";
  }
  if (schema.contains("examples") && !schema["examples"].is_array())
    return path + " examples must be an array";
  return {};
}

std::string argument_path(const std::string &parent, const std::string &name) {
  return parent.empty() ? name : parent + "." + name;
}

std::size_t utf8_code_point_count(std::string_view value) {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](char byte) {
        return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
      }));
}

std::string validate_value(const nlohmann::json &schema,
                           const nlohmann::json &value,
                           const std::string &path) {
  if (schema.contains("type")) {
    const std::string type = schema["type"].get<std::string>();
    if (!matches_type(type, value))
      return "argument '" + path + "' must be " + type;
  }
  if (schema.contains("enum")) {
    const auto &values = schema["enum"];
    if (std::find(values.begin(), values.end(), value) == values.end())
      return "argument '" + path + "' must be one of the declared enum values";
  }
  for (const std::string_view bound : {"minimum", "maximum"}) {
    if (value.is_number() && schema.contains(bound)) {
      const auto &limit = schema[bound];
      if ((bound == "minimum" && value < limit) ||
          (bound == "maximum" && value > limit))
        return "argument '" + path + "' must be " +
               (bound == "minimum" ? ">= " : "<= ") + limit.dump();
    }
  }
  if (value.is_string()) {
    const std::size_t length =
        utf8_code_point_count(value.get_ref<const std::string &>());
    for (const std::string_view bound : {"minLength", "maxLength"}) {
      if (schema.contains(bound)) {
        const auto limit = schema[bound].get<std::uint64_t>();
        if ((bound == "minLength" && length < limit) ||
            (bound == "maxLength" && length > limit))
          return "argument '" + path + "' must have " +
                 (bound == "minLength" ? "at least " : "at most ") +
                 std::to_string(limit) + " characters";
      }
    }
  }
  if (value.is_array()) {
    for (const std::string_view bound : {"minItems", "maxItems"}) {
      if (schema.contains(bound)) {
        const auto limit = schema[bound].get<std::uint64_t>();
        if ((bound == "minItems" && value.size() < limit) ||
            (bound == "maxItems" && value.size() > limit))
          return "argument '" + path + "' must contain " +
                 (bound == "minItems" ? "at least " : "at most ") +
                 std::to_string(limit) + " items";
      }
    }
  }
  if (value.is_array() && schema.contains("items")) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      std::string error =
          validate_value(schema["items"], value[index],
                         path + "[" + std::to_string(index) + "]");
      if (!error.empty())
        return error;
    }
  }
  if (!value.is_object())
    return {};

  const auto properties = schema.find("properties");
  if (schema.contains("required")) {
    for (const auto &required : schema["required"]) {
      const std::string name = required.get<std::string>();
      if (!value.contains(name))
        return "missing required argument '" + argument_path(path, name) + "'";
    }
  }
  if (schema.value("additionalProperties", true) == false) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (properties == schema.end() || !properties->contains(it.key()))
        return "argument '" + argument_path(path, it.key()) +
               "' is not allowed";
    }
  }
  if (properties != schema.end()) {
    for (auto it = properties->begin(); it != properties->end(); ++it) {
      const auto value_it = value.find(it.key());
      if (value_it == value.end())
        continue;
      std::string error = validate_value(it.value(), value_it.value(),
                                         argument_path(path, it.key()));
      if (!error.empty())
        return error;
    }
  }
  return {};
}

std::string validate_input(const ToolSpec &spec, const nlohmann::json &input) {
  if (!input.is_object())
    return "tool input must be a JSON object";

  return validate_value(spec.input_schema, input, {});
}

std::string final_text(const std::vector<Block> &blocks) {
  std::string text;
  for (const auto &block : blocks) {
    if (const auto *value = std::get_if<Text>(&block)) {
      if (!text.empty())
        text += '\n';
      text += value->text;
    }
  }
  return text;
}

} // namespace

std::string ToolRegistry::add(ToolSpec spec, ToolHandler handler) {
  if (spec.name.empty())
    return "tool name is empty";
  if (!handler)
    return "tool handler is empty";
  std::string schema_error =
      validate_schema_definition(spec.input_schema, "tool input schema");
  if (!schema_error.empty())
    return schema_error;
  if (std::any_of(
          entries_.begin(), entries_.end(),
          [&spec](const Entry &entry) { return entry.spec.name == spec.name; }))
    return "tool '" + spec.name + "' is already registered";

  specs_.push_back(spec);
  entries_.push_back({std::move(spec), std::move(handler)});
  return {};
}

const std::vector<ToolSpec> &ToolRegistry::specs() const {
  return specs_;
}

Result<std::string> ToolRegistry::execute(const std::string &name,
                                          const nlohmann::json &input) const {
  const auto entry = std::find_if(
      entries_.begin(), entries_.end(),
      [&name](const Entry &candidate) { return candidate.spec.name == name; });
  if (entry == entries_.end())
    return Error{ErrorKind::Protocol, "unknown tool '" + name + "'", 0, false};
  const std::string argument_error = validate_input(entry->spec, input);
  if (!argument_error.empty())
    return Error{ErrorKind::Protocol, argument_error, 0, false};
  return entry->handler(input);
}

Result<AgentResponse> run_agent(Client &client, Request request,
                                const ToolRegistry &tools,
                                AgentOptions options) {
  if (options.max_steps <= 0 || options.max_tool_calls < 0 ||
      options.max_repeats_per_tool_call <= 0)
    return Error{ErrorKind::Configuration, "agent limits must be positive", 0,
                 false};
  request.tools = tools.specs();
  AgentResponse result;
  std::map<std::string, std::pair<std::string, bool>> previous_results;
  std::map<std::string, int> repeat_counts;
  const auto cancelled = [] {
    return Error{ErrorKind::Cancelled, "agent run cancelled", 0, false};
  };
  const auto notify =
      [&options](const AgentEvent &event) -> std::optional<Error> {
    if (!options.on_event)
      return std::nullopt;
    try {
      options.on_event(event);
    } catch (const std::exception &error) {
      return Error{ErrorKind::Callback,
                   std::string("agent progress callback failed: ") +
                       error.what(),
                   0, false};
    } catch (...) {
      return Error{ErrorKind::Callback, "agent progress callback failed", 0,
                   false};
    }
    return std::nullopt;
  };

  for (int step = 0; step < options.max_steps; ++step) {
    if (options.stop_token.stop_requested())
      return cancelled();
    if (const auto error =
            notify({AgentEventKind::ModelRequestStarted, step + 1, {}, false}))
      return *error;
    if (options.stop_token.stop_requested())
      return cancelled();
    auto response = client.send(request);
    if (!response)
      return response.error();
    ++result.steps;
    request.messages.push_back({"assistant", response.value().content});
    if (const auto error = notify(
            {AgentEventKind::ModelResponseReceived, step + 1, {}, false}))
      return *error;
    if (options.stop_token.stop_requested())
      return cancelled();

    std::vector<Block> tool_results;
    std::vector<const ToolUse *> calls;
    for (const auto &block : response.value().content) {
      const auto *call = std::get_if<ToolUse>(&block);
      if (call)
        calls.push_back(call);
    }
    if (calls.empty()) {
      result.text = final_text(response.value().content);
      result.stop_reason = response.value().stop_reason;
      return result;
    }
    if (step + 1 >= options.max_steps)
      return Error{ErrorKind::Limit,
                   "agent step limit reached before tool execution", 0, false};
    if (result.tool_calls + static_cast<int>(calls.size()) >
        options.max_tool_calls)
      return Error{ErrorKind::Limit,
                   "tool call limit reached before tool execution", 0, false};

    auto pending_repeat_counts = repeat_counts;
    for (const ToolUse *call : calls) {
      const std::string key = call->name + "\n" + call->input.dump();
      if (previous_results.contains(key) &&
          ++pending_repeat_counts[key] >= options.max_repeats_per_tool_call)
        return Error{ErrorKind::Limit,
                     "repeat limit reached for identical tool and arguments", 0,
                     false};
    }

    for (const ToolUse *call : calls) {
      if (options.stop_token.stop_requested())
        return cancelled();
      ++result.tool_calls;
      const std::string key = call->name + "\n" + call->input.dump();
      const auto previous = previous_results.find(key);
      if (previous != previous_results.end()) {
        if (const auto error =
                notify({AgentEventKind::ToolCallRepeated, step + 1, call->name,
                        previous->second.second}))
          return *error;
        if (options.stop_token.stop_requested())
          return cancelled();
        ++repeat_counts[key];
        tool_results.push_back(
            {ToolResult{call->id,
                        "Repeated tool call was not re-executed. Earlier "
                        "result:\n" +
                            previous->second.first +
                            "\nHint: use this result or choose different "
                            "arguments.",
                        previous->second.second}});
      } else {
        if (const auto error = notify({AgentEventKind::ToolExecutionStarted,
                                       step + 1, call->name, false}))
          return *error;
        if (options.stop_token.stop_requested())
          return cancelled();
        auto tool_result = tools.execute(call->name, call->input);
        const bool is_error = !tool_result;
        if (tool_result) {
          previous_results.emplace(key,
                                   std::make_pair(tool_result.value(), false));
          tool_results.push_back(
              {ToolResult{call->id, tool_result.value(), false}});
        } else {
          previous_results.emplace(
              key, std::make_pair(tool_result.error().message, true));
          tool_results.push_back(
              {ToolResult{call->id, tool_result.error().message, true}});
        }
        if (const auto error = notify({AgentEventKind::ToolExecutionCompleted,
                                       step + 1, call->name, is_error}))
          return *error;
        if (options.stop_token.stop_requested())
          return cancelled();
      }
    }
    request.messages.push_back({"user", std::move(tool_results)});
  }
  return Error{ErrorKind::Limit, "agent step limit reached", 0, false};
}

} // namespace xoi
