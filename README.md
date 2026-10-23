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
override the endpoint. OpenAI-compatible providers can select the
`max_completion_tokens` field with `XOI_OPENAI_TOKEN_LIMIT_PARAMETER`; the
default is `max_tokens`. Set `XOI_API_KEY_HEADER` when a provider expects the
raw key in a custom header, such as `X-API-Key`.

`XOI_PROXY_URL` explicitly overrides libcurl's proxy environment. An unset
value leaves libcurl's defaults in effect; an empty value disables proxy use.
Requests can set optional `temperature` and `top_p` sampling values in
`xoi::Request`.

## Build

```sh
cmake -S . -B build && cmake --build build
```

With Ollama running locally and a model available, try the streaming example:

```sh
./build/xoi_ollama_chat
./build/xoi_ollama_chat qwen3:8b
```

Tool schema validation supports `type`, `properties`, `required`, `items`,
`enum`, boolean `additionalProperties`, numeric `minimum`/`maximum`, string
`minLength`/`maxLength`, and array `minItems`/`maxItems`. Object properties and
array items are validated recursively. String lengths count UTF-8 code points.
Unsupported schema keywords are rejected when a tool is registered instead
of being silently ignored; this is a limited subset, not a full JSON Schema
implementation. Tool handlers run in-process and are not sandboxed, so
applications must isolate them.

`Client::send_stream` supports server-sent event responses from Anthropic
Messages and OpenAI-compatible APIs, and newline-delimited JSON from Ollama.
It delivers text deltas to a callback and returns any completed tool calls
with the final response.

`run_agent` can observe model/tool lifecycle progress and accepts a
`std::stop_token` for cooperative cancellation between blocking model requests
and tool calls. Cancellation does not interrupt an active request or handler.
Progress events include tool names, but not tool arguments or results.

`ToolRegistry::add_typed<Arguments>` converts a tool's JSON input to a
caller-defined C++ type using nlohmann JSON's `from_json` customization. The
existing overload accepts an explicit `ToolSpec`. An opt-in overload generates
the root object schema from named `schema_field` member pointers:

```cpp
tools.add_typed<SearchArguments>(
    "search", "Search the index",
    [](const SearchArguments &args) -> xoi::Result<std::string> {
      return search(args.query, args.limit);
    },
    xoi::schema_field("query", &SearchArguments::query),
    xoi::schema_field("limit", &SearchArguments::limit));
```

Schema inference covers `std::string`, booleans, integral and floating-point
values, `std::optional`, `std::vector`, and fixed `std::array` fields. Optional
members are not required; fixed arrays emit equal `minItems` and `maxItems`.
Generated object schemas reject unknown fields. Since C++20 cannot reflect
ordinary member names or arbitrary `from_json` behavior, callers must name
each field and use the schema override overload for custom, nested, or enum
types. An override must match the member's JSON conversion. Unsupported
inferred field types fail at compile time. `schema_field` only builds the
schema; it does not generate a `from_json` conversion. The caller must provide
a compatible conversion, including handling optional members. Decode errors
are returned as tool errors for the model to observe.

See `xoi_host_verification` for a host-owned tool example. It returns
verification output supplied by the application; it does not run commands or
provide sandboxing, authorization, or filesystem policy.

## License

Apache-2.0. Copyright (c) 2026 coccinella-labs. See [LICENSE](LICENSE).
