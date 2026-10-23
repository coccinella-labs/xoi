#pragma once

#include <xoi/types.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace xoi {
namespace detail {

template <typename> inline constexpr bool unsupported_schema_type = false;

template <typename T> struct optional_schema_type {
  static constexpr bool value = false;
  using type = T;
};

template <typename T> struct optional_schema_type<std::optional<T>> {
  static constexpr bool value = true;
  using type = T;
};

template <typename T> struct vector_schema_type {
  static constexpr bool value = false;
};

template <typename T, typename Allocator>
struct vector_schema_type<std::vector<T, Allocator>> {
  static constexpr bool value = true;
  using type = T;
};

template <typename T> struct array_schema_type {
  static constexpr bool value = false;
};

template <typename T, std::size_t Size>
struct array_schema_type<std::array<T, Size>> {
  static constexpr bool value = true;
  using type = T;
  static constexpr std::size_t size = Size;
};

template <typename T> nlohmann::json inferred_schema() {
  using Value = std::remove_cv_t<T>;
  if constexpr (optional_schema_type<Value>::value) {
    return inferred_schema<typename optional_schema_type<Value>::type>();
  } else if constexpr (std::is_same_v<Value, std::string>) {
    return {{"type", "string"}};
  } else if constexpr (std::is_same_v<Value, bool>) {
    return {{"type", "boolean"}};
  } else if constexpr (std::is_integral_v<Value>) {
    return {{"type", "integer"}};
  } else if constexpr (std::is_floating_point_v<Value>) {
    return {{"type", "number"}};
  } else if constexpr (vector_schema_type<Value>::value) {
    return {
        {"type", "array"},
        {"items", inferred_schema<typename vector_schema_type<Value>::type>()}};
  } else if constexpr (array_schema_type<Value>::value) {
    constexpr auto size = array_schema_type<Value>::size;
    return {
        {"type", "array"},
        {"items", inferred_schema<typename array_schema_type<Value>::type>()},
        {"minItems", size},
        {"maxItems", size}};
  } else {
    static_assert(unsupported_schema_type<Value>,
                  "xoi cannot infer a JSON Schema for this C++ field type; "
                  "provide an explicit schema override");
  }
}

template <typename Owner> struct SchemaField {
  using owner_type = Owner;

  std::string name;
  nlohmann::json schema;
  bool required;
};

} // namespace detail

/// Declares a tool input field and infers its schema from the C++ member type.
/// Optional members are not required in the generated root object schema.
template <typename Member, typename Owner>
detail::SchemaField<Owner> schema_field(std::string name, Member Owner::*) {
  using Value = std::remove_cv_t<Member>;
  return {std::move(name), detail::inferred_schema<Value>(),
          !detail::optional_schema_type<Value>::value};
}

/// Declares a field with an explicit schema for custom, nested, or enum types.
/// The supplied schema must agree with the C++ member's JSON conversion.
template <typename Member, typename Owner>
detail::SchemaField<Owner> schema_field(std::string name, Member Owner::*,
                                        nlohmann::json schema) {
  using Value = std::remove_cv_t<Member>;
  return {std::move(name), std::move(schema),
          !detail::optional_schema_type<Value>::value};
}

} // namespace xoi
