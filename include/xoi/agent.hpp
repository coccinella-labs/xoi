#pragma once

#include <xoi/client.hpp>
#include <xoi/types.hpp>

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace xoi {

/// Executes a tool input and returns its text result or an error.
using ToolHandler = std::function<Result<std::string>(const nlohmann::json &)>;

/// Stores tool specifications and in-process handlers. Schema validation is
/// basic and checks required fields and declared property types only. Handlers
/// are not sandboxed; the application is responsible for isolating them.
class ToolRegistry {
public:
  /// Registers a tool; returns an error string when registration is invalid.
  std::string add(ToolSpec spec, ToolHandler handler);
  /// Returns the registered tool specifications in registration order.
  const std::vector<ToolSpec> &specs() const;
  /// Validates and executes a registered tool call.
  Result<std::string> execute(const std::string &name,
                              const nlohmann::json &input) const;

private:
  struct Entry {
    ToolSpec spec;
    ToolHandler handler;
  };
  std::vector<Entry> entries_;
  std::vector<ToolSpec> specs_;
};

/// Identifies a lifecycle event emitted during an agent run.
enum class AgentEventKind {
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
};

/// Bounds an agent run and optionally observes progress and cancellation.
struct AgentOptions {
  int max_steps = 8;
  int max_tool_calls = 32;
  int max_repeats_per_tool_call = 3;
  std::stop_token stop_token;
  std::function<void(const AgentEvent &)> on_event;
};

/// Summarizes a completed agent run.
struct AgentResponse {
  std::string text;
  std::string stop_reason;
  int steps = 0;
  int tool_calls = 0;
};

/// Runs a blocking tool-call conversation using the supplied registry.
/// Cancellation is checked between model requests and tool calls; it does not
/// interrupt an active transport request or tool handler.
Result<AgentResponse> run_agent(Client &client, Request request,
                                const ToolRegistry &tools,
                                AgentOptions options = {});

} // namespace xoi
