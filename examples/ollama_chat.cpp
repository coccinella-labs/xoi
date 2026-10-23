#include <xoi/client.hpp>

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
  const std::string model = argc > 1 ? argv[1] : "llama3.2";
  xoi::Client client{xoi::Config::from_env(xoi::Provider::Ollama)};

  xoi::Request request;
  request.model = model;
  request.messages = {{"user", {xoi::Text{"Say hello in one sentence."}}}};

  const auto result = client.send_stream(request, [](std::string_view delta) {
    std::cout << delta << std::flush;
  });
  if (!result) {
    std::cerr << "xoi Ollama request failed: " << result.error().message
              << '\n';
    return 1;
  }
  std::cout << '\n';
}
