# xoi

xoi is a C++20 SDK for Anthropic Messages, OpenAI-compatible APIs, and native
Ollama chat.

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
For Ollama, use `Config::from_env(xoi::Provider::Ollama)`; it defaults to
`http://localhost:11434` and does not require an API key. `XOI_BASE_URL` can
override the endpoint.

## Build

```sh
cmake -S . -B build && cmake --build build
```

With Ollama running locally and a model available, try the streaming example:

```sh
./build/xoi_ollama_chat
./build/xoi_ollama_chat qwen3:8b
```

Tool schema validation checks required fields and declared property types only.
Tool handlers run in-process and are not sandboxed, so applications must
isolate them.

`Client::send_stream` supports server-sent event responses from Anthropic
Messages and OpenAI-compatible APIs, and newline-delimited JSON from Ollama.
It delivers text deltas to a callback and returns any completed tool calls
with the final response.

`run_agent` can observe model/tool lifecycle progress and accepts a
`std::stop_token` for cooperative cancellation between blocking model requests
and tool calls. Cancellation does not interrupt an active request or handler.
Progress events include tool names, but not tool arguments or results.

## License

Apache-2.0. Copyright (c) 2026 coccinella-labs. See [LICENSE](LICENSE).
