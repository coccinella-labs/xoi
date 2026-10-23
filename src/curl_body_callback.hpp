#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace xoi::detail {

struct BodyCallbackState {
  std::string body;
  std::size_t max_bytes = 0;
  std::size_t received_bytes = 0;
  bool too_large = false;
  bool callback_failed = false;
  std::function<void(std::string &, std::string_view)> append =
      [](std::string &target, std::string_view chunk) {
        target.append(chunk.data(), chunk.size());
      };
};

inline std::size_t append_body(char *data, std::size_t size, std::size_t count,
                               void *target) noexcept {
  auto &state = *static_cast<BodyCallbackState *>(target);
  if (count != 0 && size > static_cast<std::size_t>(-1) / count) {
    state.callback_failed = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes >
      state.max_bytes - (std::min)(state.received_bytes, state.max_bytes)) {
    state.too_large = true;
    return 0;
  }
  try {
    state.append(state.body, std::string_view(data, bytes));
    state.received_bytes += bytes;
    return bytes;
  } catch (...) {
    state.callback_failed = true;
    return 0;
  }
}

} // namespace xoi::detail
