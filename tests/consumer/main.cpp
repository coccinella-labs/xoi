#include <xoi/agent.hpp>

#include <string>

int main() {
  const auto config = xoi::Config::from_env(xoi::Provider::Ollama);
  if (config.base_url.empty())
    return 1;

  xoi::ToolRegistry tools;
  const std::string registration_error = tools.add(
      {"echo",
       "Return the supplied value",
       {{"type", "object"},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"required", {"value"}}}},
      [](const nlohmann::json &input) {
        return xoi::Result<std::string>(input["value"].get<std::string>());
      });
  if (!registration_error.empty())
    return 2;

  const auto result = tools.execute("echo", {{"value", "package works"}});
  return result && result.value() == "package works" ? 0 : 3;
}
