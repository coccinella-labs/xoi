#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xoi {

/// Identifies a failure reported by the HTTP transport.
enum class TransportFailure : std::uint8_t {
  None,
  Initialization,
  HandleCreation,
  HeaderAllocation,
  OptionConfiguration,
  Callback,
  Connection,
  Timeout,
  Other,
};

/// An HTTP response or a transport-level failure.
struct HttpResponse {
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string transport_error;
  bool response_too_large = false;
  bool response_callback_failed = false;
  TransportFailure transport_failure = TransportFailure::None;
  std::size_t response_bytes_received = 0;

  /// Returns a response header value using case-insensitive name matching.
  std::string header(const std::string &name) const;
};

/// Injectable HTTP transport used by the client.
class Transport {
public:
  /// Destroys the transport implementation.
  virtual ~Transport() = default;
  /// Sends one POST request and returns its HTTP or transport result.
  virtual HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec, std::size_t max_response_bytes) = 0;
};

/// The default libcurl-backed HTTP transport.
class CurlTransport final : public Transport {
public:
  /// Sends one POST request with the provided timeout and headers.
  HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec, std::size_t max_response_bytes) override;
};

} // namespace xoi
