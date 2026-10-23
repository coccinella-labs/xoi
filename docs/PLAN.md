# xoi project plan

## Direction

xoi is a small C++20 SDK for provider requests, streaming responses, and
host-controlled tool use. Its purpose is to let applications reuse reliable
model I/O and agent primitives without adopting a particular terminal UI,
application architecture, or tool policy.

## Current baseline

- Blocking requests for Anthropic Messages, OpenAI-compatible APIs, and native
  Ollama `/api/chat`.
- Server-sent event streaming for Anthropic and OpenAI-compatible responses,
  plus NDJSON streaming for Ollama.
- A bounded tool-call agent loop with host-owned tools, cooperative
  cancellation, and progress events.
- An installed CMake package, examples, and tests covering provider parsing,
  transport failures, streaming, and tool execution.

## Priorities

1. **Make provider behavior explicit.** Keep provider-specific request and
   response handling behind a consistent API, while documenting differences
   that cannot be normalized. Native Ollama chat is implemented; prove it in
   the agentware integration prototype before expanding provider scope.
2. **Make agent runs controllable.** Build on the existing bounded tool loop
   with clear cancellation, resource limits, and useful progress reporting.
   Keep tool execution in the host application; xoi does not sandbox handlers.
3. **Prove integration before expanding scope.** The first real consumer is
   agentware. Its integration is an opt-in prototype until the parity gates
   below pass; an xoi-only test suite is not evidence of agentware readiness.
4. **Stabilize the public C++ API.** Keep examples aligned with the installed
   library, document compatibility expectations, and make API changes
   deliberately as supported consumers emerge.
5. **Defer broader modalities and orchestration.** Image/audio inputs,
   asynchronous APIs, and multi-agent orchestration should follow demonstrated
   consumer needs rather than be added speculatively.

## Agentware integration milestone

**Status: prototype in progress.** The first adapter targets agentware's
existing local Ollama path. It uses xoi for provider HTTP/protocol handling
while agentware remains in charge of prompt construction, JSON tool-call
policy, consent, path and secret checks, tool execution, UI, and persistence.
The adapter is opt-in; the existing provider path remains available as the
default during evaluation.

1. **Ollama adapter prototype.** Build agentware against an installed xoi
   package behind an explicit CMake option. Preserve agentware's current
   request and response contract, including its model-driven tool loop. Prove
   the adapter with the existing hermetic Ollama mock and the complete tool
   loop golden suite. Do not download a model for this gate.
2. **Behavior and safety parity.** Compare prompts, provider settings,
   timeouts, errors, streaming behavior, and tool-loop outcomes between the
   existing and xoi-backed paths. Keep safety decisions in agentware and test
   that refusals, consent, containment, and secret filtering are unchanged.
3. **Broader providers.** Add hosted providers only after their endpoint,
   authentication, request-parameter, timeout, retry, and streaming differences
   are represented and covered by provider-specific mocks. Cerebras' distinct
   authentication and completion parameters must not be silently normalized.
4. **Production decision.** Do not make the adapter the default or claim
   production readiness until the integration and parity suites pass on the
   supported platforms, a real local Ollama run is authorized and recorded,
   and maintainers have reviewed the operational and security boundaries.

## Boundaries

xoi owns provider protocol handling and reusable request/response and
tool-call primitives. The embedding application owns credentials policy,
tool permissions, user consent, sandboxing, persistence, and UI. Model output
is untrusted input; registering a tool does not make its handler safe.

## Completion criteria

Each new capability should have deterministic tests for successful and failure
paths, documented provider and security boundaries, and a working example.
Integration into another application is a separate milestone and must not be
claimed until exercised against that application's real interfaces.
