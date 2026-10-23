#pragma once

#include <curl/curl.h>

namespace xoi::detail {

template <typename Value>
CURLcode checked_set_option(CURL *handle, CURLoption option, Value value) {
  return curl_easy_setopt(handle, option, value);
}

} // namespace xoi::detail
