#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xoi {

/// Receives an arbitrary response chunk, valid only during the callback.
using StreamChunkHandler = std::function<void(std::string_view)>;

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
  /// Sends a POST and forwards successful response chunks as they arrive.
  /// Transports that do not support incremental delivery may use this
  /// compatibility implementation.
  virtual HttpResponse
  post_stream(const std::string &url, const std::string &body,
              const std::vector<std::pair<std::string, std::string>> &headers,
              int timeout_sec, std::size_t max_response_bytes,
              const StreamChunkHandler &on_chunk) {
    HttpResponse response =
        post(url, body, headers, timeout_sec, max_response_bytes);
    if (response.transport_error.empty() && response.status >= 200 &&
        response.status < 300 && !response.response_too_large &&
        !response.response_callback_failed) {
      try {
        on_chunk(response.body);
        if (response.response_bytes_received == 0)
          response.response_bytes_received = response.body.size();
      } catch (const std::exception &error) {
        response.response_callback_failed = true;
        response.transport_failure = TransportFailure::Callback;
        response.transport_error =
            std::string("stream callback failed: ") + error.what();
      } catch (...) {
        response.response_callback_failed = true;
        response.transport_failure = TransportFailure::Callback;
        response.transport_error = "stream callback failed";
      }
    }
    return response;
  }
};

/// The default libcurl-backed HTTP transport.
class CurlTransport final : public Transport {
public:
  /// An unset proxy inherits libcurl's environment; an empty proxy disables it.
  explicit CurlTransport(std::optional<std::string> proxy_url = std::nullopt);

  /// Sends one POST request with the provided timeout and headers.
  HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec, std::size_t max_response_bytes) override;
  HttpResponse
  post_stream(const std::string &url, const std::string &body,
              const std::vector<std::pair<std::string, std::string>> &headers,
              int timeout_sec, std::size_t max_response_bytes,
              const StreamChunkHandler &on_chunk) override;

private:
  std::optional<std::string> proxy_url_;
};

} // namespace xoi
