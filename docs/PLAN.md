# xoi project plan

## Direction

xoi is a small C++20 SDK for provider requests, streaming responses, and
host-controlled tool use. Its purpose is to let applications reuse reliable
model I/O and agent primitives without adopting a particular terminal UI,
application architecture, or tool policy.

## Current baseline

- Blocking requests for Anthropic Messages and OpenAI-compatible APIs.
- Server-sent event streaming for text and tool-call responses.
- A bounded tool-call agent loop with a host-owned tool registry.
- CMake build and tests covering provider parsing, transport failures, and
  tool execution.

## Priorities

1. **Make provider behavior explicit.** Keep provider-specific request and
   response handling behind a consistent API, while documenting differences
   that cannot be normalized. Add support for a provider protocol when a real
   consumer needs it; native Ollama chat is the first candidate for
   agentware integration.
2. **Make agent runs controllable.** Build on the existing bounded tool loop
   with clear cancellation, resource limits, and useful progress reporting.
   Keep tool execution in the host application; xoi does not sandbox handlers.
3. **Prove integration before expanding scope.** Exercise the SDK through
   deterministic mock-provider tests and a small agentware adapter prototype.
   Preserve agentware's existing application policy and user experience until
   the adapter demonstrates provider and tool-loop parity.
4. **Stabilize the public C++ API.** Keep examples aligned with the installed
   library, document compatibility expectations, and make API changes
   deliberately as supported consumers emerge.
5. **Defer broader modalities and orchestration.** Image/audio inputs,
   asynchronous APIs, and multi-agent orchestration should follow demonstrated
   consumer needs rather than be added speculatively.

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
