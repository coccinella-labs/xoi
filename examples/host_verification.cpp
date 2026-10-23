#include <xoi/agent.hpp>

#include <iostream>
#include <string>
#include <utility>

namespace {

struct VerificationRequest {
  std::string target;
};

void from_json(const nlohmann::json &json, VerificationRequest &request) {
  json.at("target").get_to(request.target);
}

} // namespace

int main(int argc, char **argv) {
  const std::string model = argc > 1 ? argv[1] : "llama3.2";
  const std::string verification_output =
      argc > 2 ? argv[2] : "No verification output was supplied by the host.";

  xoi::ToolRegistry tools;
  const std::string registration_error = tools.add_typed<VerificationRequest>(
      {"get_verification_output",
       "Return output already collected by the host for build or tests.",
       {{"type", "object"},
        {"properties",
         {{"target", {{"type", "string"}, {"enum", {"build", "tests"}}}}}},
        {"required", {"target"}}}},
      [&verification_output](
          const VerificationRequest &request) -> xoi::Result<std::string> {
        if (request.target != "build" && request.target != "tests")
          return xoi::Error{xoi::ErrorKind::Protocol,
                            "target must be 'build' or 'tests'", 0, false};
        return verification_output;
      });
  if (!registration_error.empty()) {
    std::cerr << registration_error << '\n';
    return 1;
  }

  xoi::Client client{xoi::Config::from_env(xoi::Provider::Ollama)};
  xoi::Request request;
  request.model = model;
  request.system =
      "Use host-provided tool output as evidence. Do not claim you ran a "
      "build or test unless the output says it happened.";
  request.messages = {
      {"user",
       {xoi::Text{"Use get_verification_output for the tests target and "
                  "summarize the supplied result."}}}};

  const auto result = xoi::run_agent(client, std::move(request), tools);
  if (!result) {
    std::cerr << "xoi host verification example failed: "
              << result.error().message << '\n';
    return 1;
  }
  std::cout << result.value().text << '\n';
}
