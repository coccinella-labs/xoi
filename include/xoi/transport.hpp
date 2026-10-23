#pragma once

#include <string>
#include <utility>
#include <vector>

namespace xoi {

/// An HTTP response or a transport-level failure.
struct HttpResponse {
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string transport_error;

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
       int timeout_sec) = 0;
};

/// The default libcurl-backed HTTP transport.
class CurlTransport final : public Transport {
public:
  /// Sends one POST request with the provided timeout and headers.
  HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec) override;
};

} // namespace xoi
