#pragma once

#include <xoi/client.hpp>
#include <xoi/schema.hpp>
#include <xoi/types.hpp>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace xoi {

/// Executes a tool input and returns its text result or an error.
using ToolHandler = std::function<Result<std::string>(const nlohmann::json &)>;
/// Executes a tool input and receives the agent's cooperative stop token.
using CancellableToolHandler =
    std::function<Result<std::string>(const nlohmann::json &, std::stop_token)>;

/// Stores tool specifications and in-process handlers. Schema validation
/// supports a documented JSON Schema subset, including recursive types,
/// numeric bounds, and string/array lengths. Handlers are not sandboxed; the
/// application is responsible for isolating them. Cancellable handlers must
/// observe the supplied stop token to stop work already in progress.
class ToolRegistry {
public:
  /// Registers a tool; returns an error string when registration is invalid.
  std::string add(ToolSpec spec, ToolHandler handler);
  /// Registers a tool handler that can cooperatively observe cancellation.
  std::string add(ToolSpec spec, CancellableToolHandler handler);
  /// Registers a tool whose JSON input converts to a caller-defined C++ type.
  /// The schema remains explicit and must agree with the JSON conversion.
  template <typename Arguments>
  std::string
  add_typed(const ToolSpec &spec,
            std::function<Result<std::string>(const Arguments &)> handler) {
    if (!handler)
      return "tool handler is empty";
    return add(spec, [handler =
                          std::move(handler)](const nlohmann::json &input) {
      try {
        const Arguments arguments = input.get<Arguments>();
        return handler(arguments);
      } catch (const nlohmann::json::exception &error) {
        return Result<std::string>(Error{
            ErrorKind::Protocol,
            std::string("tool arguments could not be decoded: ") + error.what(),
            0, false});
      }
    });
  }
  /// Registers a typed tool handler that can cooperatively observe
  /// cancellation.
  template <typename Arguments>
  std::string add_typed(
      const ToolSpec &spec,
      std::function<Result<std::string>(const Arguments &, std::stop_token)>
          handler) {
    if (!handler)
      return "tool handler is empty";
    return add(spec, [handler =
                          std::move(handler)](const nlohmann::json &input,
                                              std::stop_token stop_token) {
      try {
        const Arguments arguments = input.get<Arguments>();
        return handler(arguments, stop_token);
      } catch (const nlohmann::json::exception &error) {
        return Result<std::string>(Error{
            ErrorKind::Protocol,
            std::string("tool arguments could not be decoded: ") + error.what(),
            0, false});
      }
    });
  }
  /// Registers a typed tool and infers common field schemas from member types.
  /// C++20 cannot reflect member names, so every field must be declared with
  /// schema_field. Use its schema override for custom field types.
  template <typename Arguments, typename... Fields>
  std::string
  add_typed(std::string name, std::string description,
            std::function<Result<std::string>(const Arguments &)> handler,
            Fields... fields) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    bool duplicate_name = false;
    const auto add_field = [&]<typename Field>(Field field) {
      static_assert(std::is_same_v<typename Field::owner_type, Arguments>,
                    "all schema fields must belong to the typed arguments");
      if (properties.contains(field.name))
        duplicate_name = true;
      properties[field.name] = std::move(field.schema);
      if (field.required)
        required.push_back(std::move(field.name));
    };
    (add_field(std::move(fields)), ...);
    if (duplicate_name)
      return "tool schema field names must be unique";

    ToolSpec spec;
    spec.name = std::move(name);
    spec.description = std::move(description);
    spec.input_schema = {{"type", "object"},
                         {"properties", std::move(properties)},
                         {"additionalProperties", false}};
    if (!required.empty())
      spec.input_schema["required"] = std::move(required);
    return add_typed<Arguments>(spec, std::move(handler));
  }
  /// Registers a typed tool with inferred field schemas and a cancellable
  /// handler.
  template <typename Arguments, typename... Fields>
  std::string add_typed(
      std::string name, std::string description,
      std::function<Result<std::string>(const Arguments &, std::stop_token)>
          handler,
      Fields... fields) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    bool duplicate_name = false;
    const auto add_field = [&]<typename Field>(Field field) {
      static_assert(std::is_same_v<typename Field::owner_type, Arguments>,
                    "all schema fields must belong to the typed arguments");
      if (properties.contains(field.name))
        duplicate_name = true;
      properties[field.name] = std::move(field.schema);
      if (field.required)
        required.push_back(std::move(field.name));
    };
    (add_field(std::move(fields)), ...);
    if (duplicate_name)
      return "tool schema field names must be unique";

    ToolSpec spec;
    spec.name = std::move(name);
    spec.description = std::move(description);
    spec.input_schema = {{"type", "object"},
                         {"properties", std::move(properties)},
                         {"additionalProperties", false}};
    if (!required.empty())
      spec.input_schema["required"] = std::move(required);
    return add_typed<Arguments>(spec, std::move(handler));
  }
  /// Returns the registered tool specifications in registration order.
  const std::vector<ToolSpec> &specs() const;
  /// Validates and executes a registered tool call.
  Result<std::string> execute(const std::string &name,
                              const nlohmann::json &input,
                              const std::stop_token &stop_token = {}) const;

private:
  struct Entry {
    ToolSpec spec;
    CancellableToolHandler handler;
  };
  std::vector<Entry> entries_;
  std::vector<ToolSpec> specs_;
};

/// Identifies a lifecycle event emitted during an agent run.
enum class AgentEventKind : std::uint8_t {
  ModelRequestStarted,
  ModelResponseReceived,
  ToolExecutionStarted,
  ToolExecutionCompleted,
  ToolCallRepeated,
};

/// Progress data that does not include tool arguments or results.
struct AgentEvent {
  AgentEventKind kind;
  int step = 0;
  std::string tool_name;
  bool is_error = false;
  /// Usage reported for this model response; absent when unavailable.
  std::optional<Usage> usage;
};

/// Bounds an agent run and optionally observes progress, text, and
/// cancellation.
struct AgentOptions {
  int max_steps = 8;
  int max_tool_calls = 32;
  int max_repeats_per_tool_call = 3;
  /// Cancels provider requests and is passed to token-aware tool handlers.
  std::stop_token stop_token;
  std::function<void(const AgentEvent &)> on_event;
  /// Receives streamed text deltas. When set, model turns use send_stream;
  /// partial text is included in Error::partial_history on cancellation.
  std::function<void(std::string_view)> on_text_delta;
};

/// Summarizes a completed agent run.
struct AgentResponse {
  std::string text;
  std::string stop_reason;
  int steps = 0;
  int tool_calls = 0;
  /// Per-field totals across model turns. A count is unknown if any turn
  /// omitted that count.
  Usage usage;
};

/// Runs a blocking tool-call conversation using the supplied registry.
/// Cancellation interrupts CurlTransport requests and is checked between
/// model requests and tool calls. Cancelled errors contain resumable history;
/// token-aware tool handlers must cooperate.
Result<AgentResponse> run_agent(Client &client, Request request,
                                const ToolRegistry &tools,
                                AgentOptions options = {});

} // namespace xoi
