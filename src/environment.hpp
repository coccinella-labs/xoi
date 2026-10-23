#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace xoi::detail {

inline std::string read_environment_variable(const char *name) {
#ifdef _MSC_VER
  char *raw_value = nullptr;
  std::size_t value_size = 0;
  const auto status = _dupenv_s(&raw_value, &value_size, name);
  const std::unique_ptr<char, decltype(&std::free)> value(raw_value,
                                                          &std::free);
  if (status != 0 || !value)
    return {};
  return value.get();
#else
  const char *value = std::getenv(name);
  return value ? value : "";
#endif
}

inline std::optional<std::string>
read_optional_environment_variable(const char *name) {
#ifdef _MSC_VER
  char *raw_value = nullptr;
  std::size_t value_size = 0;
  const auto status = _dupenv_s(&raw_value, &value_size, name);
  const std::unique_ptr<char, decltype(&std::free)> value(raw_value,
                                                          &std::free);
  if (status != 0 || !value)
    return std::nullopt;
  return std::string(value.get());
#else
  const char *value = std::getenv(name);
  if (!value)
    return std::nullopt;
  return std::string(value);
#endif
}

} // namespace xoi::detail
