#include <xoi/agent.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace xoi {
namespace {

std::string validate_input(const ToolSpec &spec, const nlohmann::json &input) {
  if (!input.is_object())
    return "tool input must be a JSON object";

  if (spec.input_schema.contains("required") &&
      spec.input_schema["required"].is_array()) {
    for (const auto &required : spec.input_schema["required"]) {
      if (!required.is_string())
        continue;
      const std::string name = required.get<std::string>();
      if (!input.contains(name))
        return "missing required argument '" + name + "'";
    }
  }

  const auto properties = spec.input_schema.find("properties");
  if (properties == spec.input_schema.end() || !properties->is_object())
    return {};
  for (auto it = input.begin(); it != input.end(); ++it) {
    const auto property = properties->find(it.key());
    if (property == properties->end() || !property->is_object() ||
        !property->contains("type") || !(*property)["type"].is_string())
      continue;
    const std::string type = (*property)["type"].get<std::string>();
    const auto &value = it.value();
    const bool valid = type == "string"    ? value.is_string()
                       : type == "integer" ? value.is_number_integer()
                       : type == "number"  ? value.is_number()
                       : type == "boolean" ? value.is_boolean()
                       : type == "object"  ? value.is_object()
                       : type == "array"   ? value.is_array()
                       : type == "null"    ? value.is_null()
                                           : true;
    if (!valid)
      return "argument '" + it.key() + "' must be " + type;
  }
  return {};
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
  if (!spec.input_schema.is_object())
    return "tool input schema must be an object";
  if (spec.input_schema.contains("properties") &&
      !spec.input_schema["properties"].is_object())
    return "tool input schema properties must be an object";
  if (spec.input_schema.contains("required")) {
    if (!spec.input_schema["required"].is_array())
      return "tool input schema required must be an array";
    for (const auto &required : spec.input_schema["required"]) {
      if (!required.is_string())
        return "tool input schema required entries must be strings";
    }
  }
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

  for (int step = 0; step < options.max_steps; ++step) {
    auto response = client.send(request);
    if (!response)
      return response.error();
    ++result.steps;
    request.messages.push_back({"assistant", response.value().content});

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
      ++result.tool_calls;
      const std::string key = call->name + "\n" + call->input.dump();
      const auto previous = previous_results.find(key);
      if (previous != previous_results.end()) {
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
        auto tool_result = tools.execute(call->name, call->input);
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
      }
    }
    request.messages.push_back({"user", std::move(tool_results)});
  }
  return Error{ErrorKind::Limit, "agent step limit reached", 0, false};
}

} // namespace xoi
