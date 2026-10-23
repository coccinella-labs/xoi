#pragma once

#include <xoi/client.hpp>
#include <xoi/types.hpp>

#include <functional>
#include <string>
#include <vector>

namespace xoi {

using ToolHandler = std::function<Result<std::string>(const nlohmann::json &)>;

class ToolRegistry {
public:
  std::string add(ToolSpec spec, ToolHandler handler);
  const std::vector<ToolSpec> &specs() const;
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

struct AgentOptions {
  int max_steps = 8;
  int max_tool_calls = 32;
  int max_repeats_per_tool_call = 3;
};

struct AgentResponse {
  std::string text;
  std::string stop_reason;
  int steps = 0;
  int tool_calls = 0;
};

Result<AgentResponse> run_agent(Client &client, Request request,
                                const ToolRegistry &tools,
                                AgentOptions options = {});

} // namespace xoi
