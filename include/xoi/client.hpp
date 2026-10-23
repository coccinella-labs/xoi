#pragma once

#include <xoi/transport.hpp>
#include <xoi/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace xoi {

enum class ErrorKind : std::uint8_t {
  Configuration,
  Transport,
  Http,
  Protocol,
  Limit,
};

struct Error {
  ErrorKind kind = ErrorKind::Protocol;
  std::string message;
  int status = 0;
  bool retryable = false;
};

template <typename T> class Result {
public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : value_(std::move(error)) {}

  bool has_value() const {
    return std::holds_alternative<T>(value_);
  }
  explicit operator bool() const {
    return has_value();
  }
  const T &value() const {
    return std::get<T>(value_);
  }
  T &value() {
    return std::get<T>(value_);
  }
  const Error &error() const {
    return std::get<Error>(value_);
  }

private:
  std::variant<T, Error> value_;
};

struct Config {
  Provider provider = Provider::Anthropic;
  std::string api_key;
  std::string base_url;
  int timeout_sec = 60;
  int max_retries = 3;
  int initial_backoff_ms = 250;
  int max_backoff_sec = 8;

  static Config from_env(Provider provider = Provider::Anthropic);
};

class Client {
public:
  explicit Client(Config config,
                  std::unique_ptr<Transport> transport = nullptr);
  Result<Response> send(const Request &request) const;

private:
  Config config_;
  std::unique_ptr<Transport> transport_;
};

} // namespace xoi
