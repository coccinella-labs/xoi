#include <xoi/transport.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace xoi {
namespace {

struct CurlHandle {
  CURL *value = curl_easy_init();
  ~CurlHandle() {
    if (value)
      curl_easy_cleanup(value);
  }
};

struct HeaderList {
  curl_slist *value = nullptr;
  ~HeaderList() {
    curl_slist_free_all(value);
  }
};

void initialize_curl() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t append_body(char *data, size_t size, size_t count, void *target) {
  const size_t bytes = size * count;
  static_cast<std::string *>(target)->append(data, bytes);
  return bytes;
}

size_t append_header(char *data, size_t size, size_t count, void *target) {
  const size_t bytes = size * count;
  std::string line(data, bytes);
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    auto *headers =
        static_cast<std::vector<std::pair<std::string, std::string>> *>(target);
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value[0])))
      value.erase(value.begin());
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
      value.pop_back();
    headers->emplace_back(std::move(name), std::move(value));
  }
  return bytes;
}

} // namespace

std::string HttpResponse::header(const std::string &name) const {
  auto equal_ascii_case_insensitive = [](const std::string &left,
                                         const std::string &right) {
    if (left.size() != right.size())
      return false;
    for (size_t i = 0; i < left.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(left[i])) !=
          std::tolower(static_cast<unsigned char>(right[i])))
        return false;
    }
    return true;
  };
  for (const auto &[key, value] : headers) {
    if (equal_ascii_case_insensitive(key, name))
      return value;
  }
  return {};
}

HttpResponse CurlTransport::post(
    const std::string &url, const std::string &body,
    const std::vector<std::pair<std::string, std::string>> &headers,
    int timeout_sec) {
  HttpResponse response;
  initialize_curl();
  CurlHandle handle;
  if (!handle.value) {
    response.transport_error = "curl_easy_init failed";
    return response;
  }

  HeaderList request_headers;
  for (const auto &[name, value] : headers) {
    std::string line = name;
    line.append(": ").append(value);
    auto *appended = curl_slist_append(request_headers.value, line.c_str());
    if (!appended) {
      response.transport_error = "curl_slist_append failed";
      return response;
    }
    request_headers.value = appended;
  }

  char error_buffer[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(handle.value, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(handle.value, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.value, CURLOPT_HTTPHEADER, request_headers.value);
  curl_easy_setopt(handle.value, CURLOPT_POST, 1L);
  curl_easy_setopt(handle.value, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(handle.value, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(handle.value, CURLOPT_TIMEOUT,
                   static_cast<long>(std::max(timeout_sec, 1)));
  curl_easy_setopt(handle.value, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(handle.value, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(handle.value, CURLOPT_HEADERFUNCTION, append_header);
  curl_easy_setopt(handle.value, CURLOPT_HEADERDATA, &response.headers);

  const CURLcode code = curl_easy_perform(handle.value);
  if (code != CURLE_OK) {
    response.transport_error =
        error_buffer[0] ? error_buffer : curl_easy_strerror(code);
    return response;
  }
  long status_code = 0;
  curl_easy_getinfo(handle.value, CURLINFO_RESPONSE_CODE, &status_code);
  response.status = static_cast<int>(std::clamp(
      status_code, 0L, static_cast<long>(std::numeric_limits<int>::max())));
  return response;
}

} // namespace xoi
