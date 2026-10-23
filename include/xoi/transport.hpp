#pragma once

#include <string>
#include <utility>
#include <vector>

namespace xoi {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string transport_error;

  std::string header(const std::string &name) const;
};

class Transport {
public:
  virtual ~Transport() = default;
  virtual HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec) = 0;
};

class CurlTransport final : public Transport {
public:
  HttpResponse
  post(const std::string &url, const std::string &body,
       const std::vector<std::pair<std::string, std::string>> &headers,
       int timeout_sec) override;
};

} // namespace xoi
