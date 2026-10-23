# xoi

xoi is a C++20 SDK for Anthropic Messages and OpenAI-compatible APIs.

## Usage

```cpp
#include <xoi/client.hpp>

xoi::Client client{xoi::Config::from_env()};
xoi::Request request;
request.model = "claude-3-5-haiku-latest";
request.messages = {{"user", {xoi::Text{"Say hello."}}}};
auto response = client.send(request);
```

`Config::from_env()` reads the provider credentials from the environment.

## Build

```sh
cmake -S . -B build && cmake --build build
```

Tool schema validation checks required fields and declared property types only.
Tool handlers run in-process and are not sandboxed, so applications must
isolate them.

Streaming is planned and is not implemented yet. The license decision is
pending authorization; see issue #17.
