#pragma once

#include <xoi/transport.hpp>
#include <xoi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xoi {

/// Describes the category of an SDK error.
enum class ErrorKind : std::uint8_t {
  Configuration,
  Transport,
  Http,
  Protocol,
  Limit,
  ResponseTooLarge,
  Callback,
  Cancelled,
};

/// Describes an SDK or provider error without throwing for request failures.
struct Error {
  ErrorKind kind = ErrorKind::Protocol;
  std::string message;
  int status = 0;
  bool retryable = false;
  TransportFailure transport_failure = TransportFailure::None;
  /// Conversation available when run_agent is cancelled; tool uses without
  /// results are completed with synthetic error results.
  std::optional<std::vector<Message>> partial_history = std::nullopt;
};

/// Selects the completion-token field used by OpenAI-compatible APIs.
enum class OpenAITokenLimitParameter : std::uint8_t {
  MaxTokens,
  MaxCompletionTokens,
};

/// Holds either a successful value or an SDK error.
template <typename T> class Result {
public:
  /// Constructs a successful result.
  Result(T value) : value_(std::move(value)) {}
  /// Constructs a failed result.
  Result(Error error) : value_(std::move(error)) {}

  /// Returns true when this result contains a value.
  bool has_value() const {
    return std::holds_alternative<T>(value_);
  }
  /// Returns true when this result contains a value.
  explicit operator bool() const {
    return has_value();
  }
  /// Returns the value; call only when the result is successful.
  const T &value() const {
    return std::get<T>(value_);
  }
  /// Returns the mutable value; call only when the result is successful.
  T &value() {
    return std::get<T>(value_);
  }
  /// Returns the error; call only when the result has failed.
  const Error &error() const {
    return std::get<Error>(value_);
  }

private:
  std::variant<T, Error> value_;
};

/// Configures the provider, credentials, HTTP timeout, and retry policy.
struct Config {
  Provider provider = Provider::Anthropic;
  std::string api_key;
  /// Custom raw API-key header; empty selects the provider's default header.
  std::string api_key_header;
  std::string base_url;
  /// Unset inherits libcurl's proxy environment; empty explicitly disables it.
  std::optional<std::string> proxy_url;
  OpenAITokenLimitParameter openai_token_limit_parameter =
      OpenAITokenLimitParameter::MaxTokens;
  /// Request usage in OpenAI-compatible streams; disabled by default because
  /// some compatible servers reject stream_options.include_usage.
  bool openai_stream_include_usage = false;
  /// Send num_predict from max_tokens; false leaves the limit to Ollama.
  bool ollama_send_num_predict = true;
  int timeout_sec = 60;
  std::size_t max_response_bytes = std::size_t{8} * 1024U * 1024U;
  int max_retries = 3;
  int initial_backoff_ms = 250;
  int max_backoff_sec = 8;

  /// Loads provider credentials and optional endpoint overrides from the
  /// environment.
  static Config from_env(Provider provider = Provider::Anthropic);
};

/// Sends blocking requests to an LLM provider. A request stop token cancels
/// CurlTransport promptly; streamed text delivered before cancellation remains
/// available through the callback.
class Client {
public:
  /// Creates a client with an optional custom transport.
  explicit Client(Config config,
                  std::unique_ptr<Transport> transport = nullptr);
  /// Sends a request and returns a response or a typed error, including
  /// provider-reported token usage when available.
  Result<Response> send(const Request &request) const;
  /// Sends a streaming request, reporting each text delta as it arrives.
  /// The string view is valid only during the callback. Tool calls are
  /// returned in the completed response.
  Result<Response>
  send_stream(const Request &request,
              const std::function<void(std::string_view)> &on_text_delta) const;

private:
  Result<Response>
  send_impl(const Request &request,
            const std::function<void(std::string_view)> *on_text_delta) const;
  Config config_;
  std::unique_ptr<Transport> transport_;
};

} // namespace xoi
