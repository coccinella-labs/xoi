# xoi project plan

## Direction

xoi is a reusable C++20 SDK for model-provider I/O and host-controlled tool
loops. It should make model calls, structured tool calls, streaming, and
bounded orchestration reliable and testable for C++ applications.

xoi is not intended to reproduce the entire TypeScript or Node.js ecosystem.
It does not own a product UI, web stack, credentials policy, workspace tools,
filesystem sandbox, command sandbox, or application-specific authorization.
Those capabilities belong to the embedding application. The SDK should make
their integration safer through precise C++ interfaces, schemas, typed
results, deterministic tests, and clear examples.

## Current state

The `v0.1.0` release is published from main commit
`a3675b8c1ed6a77825bc4bb406ba40efdfb9d84b`. The repository is private and
licensed Apache-2.0. The SDK currently provides:

- Blocking request/response calls for Anthropic Messages, OpenAI-compatible
  Chat Completions, and native Ollama `/api/chat`.
- Callback-based streaming for Anthropic and OpenAI-compatible SSE, and
  Ollama NDJSON. Callbacks run synchronously; there is no asynchronous client
  API.
- C++ request, response, message, and content-block types, including native
  `tool_use` and `tool_result` representations.
- A host-supplied tool registry with JSON and typed-input handlers, recursive
  validation for the documented JSON Schema subset, and a bounded synchronous
  agent loop. Typed handlers use nlohmann JSON conversions; schemas remain
  explicitly declared by the application.
- Retry/backoff for selected rate-limit, server, connection, and timeout
  failures; configurable request timeout and response-size limit.
- Optional sampling parameters, custom API-key headers, OpenAI-compatible
  completion-token field selection, and explicit libcurl proxy configuration.
- Injectable transport interfaces, deterministic mock-transport tests,
  examples, and an installed CMake package.

The current interface is a useful provider and tool-loop foundation, not a
complete coding-agent framework or a production-proven agentware integration.

## Capability audit

| Area | Status | What exists and what remains |
|---|---|---|
| C++ type guardrails | Partial | Public structs, enums, `std::variant` content blocks, `Result<T>`, and typed tool handlers provide compile-time structure. Tool input types are selected by the host and decoded through nlohmann JSON; schemas are still explicit rather than generated from C++ types. |
| Structured tool calling | Done | Anthropic, OpenAI-compatible, and Ollama adapters parse native tool calls and serialize tool results. The host supplies tools and decides what may execute. |
| Tool schema validation | Partial | Recursive object/array validation supports type, properties, required, items, enum, and boolean additionalProperties; unsupported keywords are rejected rather than ignored. Typed handlers deserialize into host-defined C++ types and return decode failures as tool errors. Full JSON Schema and schema-to-C++ generation remain unsupported. |
| Provider support | Done | Anthropic Messages, generic OpenAI-compatible Chat Completions, and native Ollama chat have request/response tests. Provider-specific extensions outside these protocols are not implied. |
| Streaming | Partial | Text deltas and completed tool calls are supported for SSE and Ollama NDJSON. The API is blocking/callback-based; there is no async task/future interface, cancellation of an active request, or uniform provider-specific streaming parity beyond tested formats. |
| Retries and transport failures | Done | Selected 429/5xx and pre-response connection/timeout failures use bounded retries/backoff. Other failures are returned as typed errors. Retry behavior is tested with an injected transport. |
| Timeouts and response limits | Done | Request timeout and maximum response bytes are configurable and passed to the transport. |
| Proxy configuration | Done | `Config::proxy_url` and `XOI_PROXY_URL` explicitly override libcurl behavior; unset inherits its environment and an empty value disables proxy use. Adapters must set an empty override when preserving loopback proxy bypass. |
| Offline/local models | Partial | Ollama is a local, no-key provider. xoi does not manage model installation, discovery, readiness, or offline guarantees for other providers. |
| Coding-agent filesystem and shell tools | Missing in xoi | No built-in read/list/glob/grep/edit/write/run-command tools. A host can register its own handlers; xoi does not sandbox them or constrain paths. |
| Verify-after-edit workflow | Missing in xoi | The SDK does not automatically run builds/tests after edits or feed diagnostics back to a model. |
| Context management | Partial | Agent steps and tool calls are bounded. Conversation history is not compacted or token-budgeted by xoi. |
| Repeat detection | Done | Identical tool/argument calls are not re-executed; the earlier result and a hint are returned, repeats consume the call budget, and a repeat threshold stops the run. |
| Safety and authorization | Host-owned | xoi validates basic argument shape but does not provide workspace containment, secrets filtering, consent, or command isolation. Applications must enforce these before handlers run. |
| Cancellation and progress | Partial | `std::stop_token` and progress callbacks work between blocking requests and tool calls. They do not interrupt an active request or handler. |
| Full-stack web/serverless support | Out of scope | xoi is a native C++ SDK, not a frontend/backend language ecosystem. It should integrate with C++ applications, not attempt to duplicate Node.js platform reach. |
| C++ ecosystem and model fluency | Out of scope | C++ cannot reproduce npm's package breadth or TypeScript's training-data prevalence. xoi should keep dependencies focused, APIs discoverable, and examples precise. |

## Agentware provider parity

Agentware has nine modes: Together, Cerebras, Fireworks, Groq, DeepSeek,
OpenAI, and three local Ollama model choices. It uses `WebService::post_json`
for all model requests, builds provider payloads in `AIService`, and parses
textual JSON tool calls in its own bounded loop.

| Agentware need | xoi support | Audit result |
|---|---|---|
| Together, Fireworks, Groq, DeepSeek, and OpenAI request/response protocol | Generic OpenAI-compatible Chat Completions with configurable base URL, bearer key, messages, tools, max tokens, temperature, and top_p | Partial: protocol and request settings fit; request/error/retry parity and provider-specific behavior still need tests. |
| Cerebras endpoint, `X-API-Key`, completion-token field, `top_p`, and SSE response | Generic OpenAI-compatible API and SSE parsing, custom API-key header, selectable completion-token field, and sampling parameters | Partial: request settings are representable, but provider/error/retry parity needs evidence; agentware currently buffers the SSE response before parsing it. |
| Three Ollama model choices and local `/api/chat` | Native Ollama API, tool-call parsing, streaming, sampling parameters, explicit proxy override, and local no-key defaults | Partial for integration: preserve agentware's prompt, model selection, timeout/error behavior, and set an empty proxy override for loopback. |
| Timeout and loop retry behavior | Configurable xoi request timeout and transport retries; typed timeout errors | Partial: disable xoi internal retries when exact behavior is needed, map typed errors to agentware observations, and test retry budgets end-to-end. |
| HTTP status and malformed-response behavior | Typed HTTP, transport, protocol, callback, and size-limit errors | Partial: agentware exposes formatted strings and truncates HTTP error bodies; adapter mapping and user-visible parity are unproven. |
| Proxy behavior | Explicit override through `Config::proxy_url` or `XOI_PROXY_URL` | Covered at the SDK configuration layer; agentware must explicitly disable proxies for loopback URLs and prove that behavior in its adapter tests. |
| Response size | xoi default cap is 8 MiB and configurable; agentware model POST has no explicit body cap | Partial: adoption introduces a bound; agree on and test the intended cap. |
| Streaming visible to the application | xoi callbacks deliver text deltas; agentware returns a completed string and has no token callback at this seam | Partial: no existing user-visible token streaming is lost for most modes, but introducing it is a separate behavior change. |
| Native tool calls while preserving Agentware policy | xoi represents provider tool calls as typed blocks; agentware currently parses JSON text and applies canonicalization, consent, refusal, containment, and secret checks | Partial: use a separate flag and keep the host gate authoritative; do not call xoi `run_agent` with ungated handlers. |

There is no all-needs-done hosted provider today. Ollama is the closest
protocol-level match, but its environment/proxy and failure mapping still need
explicit parity evidence. Choose the first provider spike only after its
specific gaps are resolved or accepted, and stop at the audit report for
maintainer review before beginning that spike.

## Coding-agent fit

Agentware already has host-side repository and command capabilities, including
read, list, tree, grep, write, replace, and command tools. Its registry and
model-call gate apply policy such as path containment, secret filtering,
refusal of model-originated mutations, and consent checks. These are
application responsibilities; they must not be moved into xoi merely to make
the SDK appear self-contained.

xoi's `ToolRegistry` and `run_agent` can carry host-provided tools through a
provider-neutral tool-call cycle, but xoi does not yet prove parity with
agentware's textual tool loop. The agentware adoption work must therefore
compare behavior before enabling native tool blocks. xoi should supply
reusable primitives and a small reference example, not copy agentware's
repository tools or policy.

Coding-agent quality also needs orchestration beyond a provider call:

- Keep edits and verification under host control; provide a clear example of
  registering a tool and returning build/test output as an observation.
- Let hosts bound tool time and output. xoi bounds SDK response size and loop
  counts, but tool handlers themselves have no timeout or output cap.
- Make long-run context policy explicit. The host must decide how to trim or
  summarize history; xoi currently retains the conversation for the bounded
  run.
- Keep mutations, shell execution, checkpoints, and Git operations in the
  host's authorization boundary.

## Priorities

1. **Complete the integration audit before migration.** Document agentware's
   provider behavior, xoi capabilities, parity gaps, test seams, and build
   constraints. Do not switch a provider whose behavior is only partially
   supported.
2. **Prove one provider at a time in agentware.** Keep the old path intact and
   gate the xoi path off by default. Pin any dependency to an exact revision;
   never add credentials or private-fetch secrets to agentware CI. Preserve
   timeouts, proxy behavior, request settings, errors, and tests.
3. **Prove native tool-call parity separately.** Use a second off-by-default
   flag. Keep agentware's prompt, canonicalization, consent, refusals, path
   checks, secret filtering, and execution gate authoritative. Run the existing
   tool-loop goldens and compare observable behavior before considering a
   default change.
4. **Extend typed tools deliberately.** `ToolRegistry::add_typed` validates
   against an explicit schema, decodes a caller-defined C++ input type, and
   reports JSON decode failures as tool errors. The registry validates a
   documented recursive subset and rejects unsupported schema keywords. A
   host-owned verification example demonstrates the boundary without running
   commands. Automatic schema generation and full JSON Schema support remain
   out of scope.
5. **Verify proxy and operational behavior.** Explicit proxy configuration is
   available. Test loopback bypass, timeout propagation, retry safety, response
   limits, and transport errors at the installed-package and consumer boundary.
6. **Clarify orchestration boundaries.** Keep filesystem, process, sandbox,
   context compaction, and Git policy in the host. Improve the example only
   where it demonstrates a real host-owned integration without implying that
   xoi provides those safety policies.
7. **Defer async and broader platform scope until justified.** Callback
   streaming is available now. Add asynchronous/concurrent APIs only with a
   concrete consumer need and a clear cancellation and lifetime model.
8. **Stabilize the public API and release deliberately.** Keep the installed
   package, examples, compatibility expectations, and supported provider
   behavior aligned. Do not call the SDK production-ready for an application
   until its integration gates pass on that application's supported platforms.

## Agentware adoption gates

The agentware adapter PR is an opt-in prototype, not evidence that xoi is
already a drop-in replacement. The previously proposed audit-first migration
is the required sequence:

1. **Audit only.** Report each provider's HTTP, payload, authentication,
   timeout, retry, streaming, error, and tool-call behavior; compare it with
   xoi; identify mock-transport integration points and build/privacy effects.
   Stop for maintainer review before migration.
2. **Provider spike.** After approval, select the simplest provider only if all
   of its required behavior is covered. Keep the old implementation and add an
   off-by-default flag plus an xoi mock-transport test. Agentware's current
   Python loopback provider mocks remain the end-to-end compatibility test;
   the xoi `Transport` mock covers protocol serialization, parsing, and
   failures without network access. Add a narrow injection seam if needed to
   exercise the adapter with that transport rather than only testing the
   client in isolation.
3. **Native tool-call spike.** After separate approval, route tool calls
   through native blocks behind another off-by-default flag. Retain the
   application safety gate and run the complete tool-loop golden suite.
4. **Report and decide.** Include all checks, behavioral differences, blocked
   xoi gaps, exact dependency revision, and the recommended next provider.
   Do not migrate the remaining providers in the same spike.
5. **Promote only after evidence.** Require platform CI, provider parity,
   authorized local-model testing where applicable, and explicit maintainer
   approval before changing defaults or deleting the fallback implementation.

If an adoption blocker belongs in xoi, track and implement it in xoi first,
with its own tests and release. Agentware must consume xoi as an external
dependency; do not copy source between repositories. Because xoi is private,
stop rather than adding a token, deploy key, or other private-fetch secret to
agentware CI. Resolve package distribution/access before requiring CI to build
the optional integration. Pin any consumed artifact to the exact SDK commit
and prove that both the default-off build and opt-in build use the intended
revision. The agentware build already uses nlohmann JSON and CPR/libcurl; xoi
also uses nlohmann JSON and libcurl, so the main build cost is compiling and
linking xoi and resolving its CMake package, not introducing an unrelated
network stack. Keep that dependency opt-in so agentware's offline-first
default remains unchanged.

## Boundaries

xoi owns provider protocol handling, typed transport results, reusable message
and tool-call representations, and bounded host-controlled orchestration. The
embedding application owns provider credentials policy, tool permissions,
consent, path and secret policy, sandboxing, handler time/output limits,
persistence, UI, context retention, and verification strategy. Model output
and tool output are untrusted data; registering a tool does not make its
handler safe.

## Completion criteria

Every capability change needs deterministic success and failure tests,
documented limits, and a working example where useful. Provider compatibility
must be tested against the wire format, not inferred from a similar provider.
An integration is complete only when the consumer's existing behavior and
safety gates are preserved, its supported-platform checks pass, and
maintainers have reviewed any remaining operational limits.
