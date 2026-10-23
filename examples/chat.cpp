#include <xoi/client.hpp>

#include <iostream>
#include <string>

int main() {
  auto config = xoi::Config::from_env();
  xoi::Client client(std::move(config));

  xoi::Request request;
  request.model = "claude-3-5-haiku-latest";
  request.messages = {{"user", {xoi::Text{"Say hello in one sentence."}}}};

  const auto result = client.send(request);
  if (!result) {
    std::cerr << "xoi request failed: " << result.error().message << '\n';
    return 1;
  }
  for (const auto &block : result.value().content) {
    if (const auto *text = std::get_if<xoi::Text>(&block))
      std::cout << text->text << '\n';
  }
}
