# xoi

xoi is a C++20 SDK for Anthropic Messages and OpenAI-compatible APIs.

## Usage

```cpp
#include <iostream>
#include <xoi/client.hpp>

int main() {
  xoi::Client client{xoi::Config::from_env()};
  xoi::Request request;
  request.model = "claude-3-5-haiku-latest";
  request.messages = {{"user", {xoi::Text{"Say hello."}}}};
  auto result = client.send_stream(request, [](std::string_view delta) {
    std::cout << delta << std::flush;
  });
  if (!result)
    std::cerr << result.error().message << '\n';
}
```

`Config::from_env()` reads the provider credentials from the environment.

## Build

```sh
cmake -S . -B build && cmake --build build
```

Tool schema validation checks required fields and declared property types only.
Tool handlers run in-process and are not sandboxed, so applications must
isolate them.

`Client::send_stream` supports server-sent event responses from Anthropic
Messages and OpenAI-compatible APIs. It delivers text deltas to a callback and
returns any completed tool calls with the final response.

The license decision is pending authorization; see issue #17.
