#pragma once

#include <xoi/transport.hpp>
#include <xoi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace xoi {

/// Describes the category of an SDK error.
enum class ErrorKind : std::uint8_t {
  Configuration,
  Transport,
  Http,
  Protocol,
  Limit,
  ResponseTooLarge,
};

/// Describes an SDK or provider error without throwing for request failures.
struct Error {
  ErrorKind kind = ErrorKind::Protocol;
  std::string message;
  int status = 0;
  bool retryable = false;
  TransportFailure transport_failure = TransportFailure::None;
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
  std::string base_url;
  int timeout_sec = 60;
  std::size_t max_response_bytes = std::size_t{8} * 1024U * 1024U;
  int max_retries = 3;
  int initial_backoff_ms = 250;
  int max_backoff_sec = 8;

  /// Loads provider credentials and optional endpoint overrides from the
  /// environment.
  static Config from_env(Provider provider = Provider::Anthropic);
};

/// Sends blocking requests to an LLM provider.
class Client {
public:
  /// Creates a client with an optional custom transport.
  explicit Client(Config config,
                  std::unique_ptr<Transport> transport = nullptr);
  /// Sends a request and returns a response or a typed error.
  Result<Response> send(const Request &request) const;

private:
  Config config_;
  std::unique_ptr<Transport> transport_;
};

} // namespace xoi
