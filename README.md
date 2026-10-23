# xoi

xoi is a small C++20 SDK for Anthropic Messages and OpenAI-compatible APIs.

```cpp
#include <xoi/client.hpp>

xoi::Client client{xoi::Config::from_env()};
xoi::Request request;
request.model = "claude-3-5-haiku-latest";
request.messages = {{"user", {xoi::Text{"Say hello."}}}};
auto response = client.send(request);
```

Build: `cmake -S . -B build && cmake --build build`

TODO: Choose MIT or Apache-2.0 before the first release.
