# xoi

A small C++20 client for Anthropic Messages and OpenAI-compatible chat APIs.

```cpp
#include <xoi/client.hpp>

xoi::Client client{xoi::Config::from_env()};
xoi::Request request;
request.model = "claude-3-5-haiku-latest";
request.messages = {{"user", {xoi::Text{"Say hello."}}}};
auto response = client.send(request);
```

Set `ANTHROPIC_API_KEY` (or `OPENAI_API_KEY` for the compatible API). Build with
CMake; `xoi::xoi` is the exported target. `XOI_API_KEY` and `XOI_BASE_URL`
override the provider-specific environment defaults.

The synchronous client uses libcurl, nlohmann/json, typed message blocks, and
bounded retries for rate limits and server errors. Tests use a mock transport
and do not contact an API.

`xoi::run_agent` provides a bounded blocking tool loop with basic JSON Schema
required-field and type validation. Tool handlers execute in-process; xoi does
not sandbox them. Streaming and workspace-specific tools are not included yet.
