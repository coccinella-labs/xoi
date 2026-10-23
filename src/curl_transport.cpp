#include <xoi/transport.hpp>

#include "curl_body_callback.hpp"
#include "curl_option.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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

[[nodiscard]] CURLcode initialize_curl() {
  static CURLcode result = CURLE_FAILED_INIT;
  static std::once_flag once;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  return result;
}

struct HeaderCallbackState {
  std::vector<std::pair<std::string, std::string>> *headers;
  int *status;
  bool failed = false;
};

size_t append_header(char *data, size_t size, size_t count, void *target) {
  auto &state = *static_cast<HeaderCallbackState *>(target);
  if (count != 0 && size > static_cast<size_t>(-1) / count) {
    state.failed = true;
    return 0;
  }
  const size_t bytes = size * count;
  try {
    std::string line(data, bytes);
    if (line.rfind("HTTP/", 0) == 0) {
      const auto separator = line.find(' ');
      if (separator != std::string::npos) {
        int status = 0;
        const char *first = line.data() + separator + 1;
        const char *last = line.data() + line.size();
        const auto [end, error] = std::from_chars(first, last, status);
        if (error == std::errc{} && end != first)
          *state.status = status;
      }
      return bytes;
    }
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() &&
             std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
      while (!value.empty() &&
             std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
      state.headers->emplace_back(std::move(name), std::move(value));
    }
    return bytes;
  } catch (...) {
    state.failed = true;
    return 0;
  }
}

TransportFailure transport_failure_for(CURLcode code) {
  switch (code) {
  case CURLE_COULDNT_RESOLVE_PROXY:
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_CONNECT:
    return TransportFailure::Connection;
  case CURLE_OPERATION_TIMEDOUT:
    return TransportFailure::Timeout;
  default:
    return TransportFailure::Other;
  }
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

namespace {

HttpResponse
perform_post(const std::string &url, const std::string &body,
             const std::vector<std::pair<std::string, std::string>> &headers,
             int timeout_sec, std::size_t max_response_bytes,
             const StreamChunkHandler *on_chunk,
             const std::optional<std::string> &proxy_url) {
  HttpResponse response;
  const CURLcode global_code = initialize_curl();
  if (global_code != CURLE_OK) {
    response.transport_failure = TransportFailure::Initialization;
    response.transport_error = curl_easy_strerror(global_code);
    return response;
  }
  CurlHandle handle{curl_easy_init()};
  if (!handle.value) {
    response.transport_failure = TransportFailure::HandleCreation;
    response.transport_error = "curl_easy_init failed";
    return response;
  }

  HeaderList request_headers;
  for (const auto &[name, value] : headers) {
    std::string line = name;
    line.append(": ").append(value);
    auto *appended = curl_slist_append(request_headers.value, line.c_str());
    if (!appended) {
      response.transport_failure = TransportFailure::HeaderAllocation;
      response.transport_error = "curl_slist_append failed";
      return response;
    }
    request_headers.value = appended;
  }

  detail::BodyCallbackState body_state;
  body_state.max_bytes = max_response_bytes;
  body_state.append = [&response, on_chunk](std::string &target,
                                            std::string_view chunk) {
    if (on_chunk && response.status >= 200 && response.status < 300)
      (*on_chunk)(chunk);
    else
      target.append(chunk.data(), chunk.size());
  };
  HeaderCallbackState header_state{&response.headers, &response.status};
  char error_buffer[CURL_ERROR_SIZE] = {};
  auto set_option = [&response, handle = handle.value](
                        CURLoption option, const char *name, auto value) {
    const CURLcode code = detail::checked_set_option(handle, option, value);
    if (code == CURLE_OK)
      return true;
    response.transport_failure = TransportFailure::OptionConfiguration;
    response.transport_error = std::string("curl_easy_setopt ") + name + ": " +
                               curl_easy_strerror(code);
    return false;
  };
  if (!set_option(CURLOPT_ERRORBUFFER, "ERRORBUFFER", error_buffer) ||
      !set_option(CURLOPT_URL, "URL", url.c_str()) ||
      !set_option(CURLOPT_HTTPHEADER, "HTTPHEADER", request_headers.value) ||
      !set_option(CURLOPT_POST, "POST", 1L) ||
      !set_option(CURLOPT_POSTFIELDS, "POSTFIELDS", body.data()) ||
      !set_option(CURLOPT_POSTFIELDSIZE_LARGE, "POSTFIELDSIZE_LARGE",
                  static_cast<curl_off_t>(body.size())) ||
      !set_option(CURLOPT_TIMEOUT, "TIMEOUT",
                  static_cast<long>((std::max)(timeout_sec, 1))) ||
      !set_option(CURLOPT_WRITEFUNCTION, "WRITEFUNCTION",
                  detail::append_body) ||
      !set_option(CURLOPT_WRITEDATA, "WRITEDATA", &body_state) ||
      !set_option(CURLOPT_HEADERFUNCTION, "HEADERFUNCTION", append_header) ||
      !set_option(CURLOPT_HEADERDATA, "HEADERDATA", &header_state))
    return response;
  if (proxy_url && !set_option(CURLOPT_PROXY, "PROXY", proxy_url->c_str()))
    return response;

  const CURLcode code = curl_easy_perform(handle.value);
  response.body = std::move(body_state.body);
  response.response_bytes_received = body_state.received_bytes;
  response.response_too_large = body_state.too_large;
  response.response_callback_failed =
      body_state.callback_failed || header_state.failed;
  if (response.response_callback_failed) {
    response.transport_failure = TransportFailure::Callback;
    response.transport_error = "curl response callback failed";
  }
  if (code != CURLE_OK) {
    if (response.transport_error.empty())
      response.transport_error =
          error_buffer[0] ? error_buffer : curl_easy_strerror(code);
    if (response.transport_failure == TransportFailure::None)
      response.transport_failure = transport_failure_for(code);
    return response;
  }
  long status_code = 0;
  const CURLcode info_code =
      curl_easy_getinfo(handle.value, CURLINFO_RESPONSE_CODE, &status_code);
  if (info_code != CURLE_OK) {
    response.transport_failure = TransportFailure::Other;
    response.transport_error =
        std::string("curl_easy_getinfo RESPONSE_CODE: ") +
        curl_easy_strerror(info_code);
    return response;
  }
  const long max_status = static_cast<long>((std::numeric_limits<int>::max)());
  response.status = static_cast<int>(status_code < 0            ? 0
                                     : status_code > max_status ? max_status
                                                                : status_code);
  return response;
}

} // namespace

CurlTransport::CurlTransport(std::optional<std::string> proxy_url)
    : proxy_url_(std::move(proxy_url)) {}

HttpResponse CurlTransport::post(
    const std::string &url, const std::string &body,
    const std::vector<std::pair<std::string, std::string>> &headers,
    int timeout_sec, std::size_t max_response_bytes) {
  return perform_post(url, body, headers, timeout_sec, max_response_bytes,
                      nullptr, proxy_url_);
}

HttpResponse CurlTransport::post_stream(
    const std::string &url, const std::string &body,
    const std::vector<std::pair<std::string, std::string>> &headers,
    int timeout_sec, std::size_t max_response_bytes,
    const StreamChunkHandler &on_chunk) {
  return perform_post(url, body, headers, timeout_sec, max_response_bytes,
                      &on_chunk, proxy_url_);
}

} // namespace xoi
