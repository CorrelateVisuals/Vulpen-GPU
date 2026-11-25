#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// What a value shows as in a row of the bench (Bench.h): the same bits always the
// same text, on every library and system, and nothing a run makes anew.

namespace VP::bench {

// The handles one row shows, numbered as they first appear in it: a handle's value
// changes from run to run, its place in the row does not.
class Names {
public:
  std::size_t number(const void *handle);
  void clear();

private:
  std::vector<const void *> _seen;
};

// The text a value shows as in a row, one per type; a case adds one for an engine type as
// a specialization, which a sweep finds wherever the case writes it.
template <class T> struct Shown {
  static_assert(sizeof(T) == 0, "no Shown<T> for this type: add one to the case");
};

template <class T> std::string show(const T &value, Names &names) {
  return Shown<std::remove_cvref_t<T>>::text(value, names);
}

// Text in quotes, each byte a terminal would obey or another file read otherwise
// escaped. bench.py cuts a long one to its ends and a hash, once it has named the
// folders a run makes anew in it.
std::string escaped(std::string_view text);
std::uint64_t hash_of(std::string_view text);

// The shortest text that reads back the same bits, as std::to_chars writes it, with nan,
// the infinities and -0 spelled alike by every standard library.
template <class T> std::string number_text(T value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(value))
      return std::signbit(value) ? "-nan" : "nan";
    if (std::isinf(value))
      return value < 0 ? "-inf" : "inf";
  }
  std::array<char, 64> text{};
  const auto end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
  return {text.data(), end};
}

template <> struct Shown<bool> {
  static std::string text(bool value, Names &) {
    return value ? "true" : "false";
  }
};
template <> struct Shown<char> {
  static std::string text(char value, Names &) {
    return escaped(std::string_view(&value, 1));
  }
};
template <class T>
  requires std::integral<T> || std::floating_point<T>
struct Shown<T> {
  static std::string text(T value, Names &) {
    return number_text(value);
  }
};
// An engine enum shows by its names once its case adds them.
template <class T>
  requires std::is_enum_v<T>
struct Shown<T> {
  static std::string text(T value, Names &) {
    return "enum " + number_text(static_cast<std::underlying_type_t<T>>(value));
  }
};
template <> struct Shown<std::string> {
  static std::string text(const std::string &value, Names &) {
    return escaped(value);
  }
};
template <> struct Shown<std::string_view> {
  static std::string text(std::string_view value, Names &) {
    return escaped(value);
  }
};
template <> struct Shown<const char *> {
  static std::string text(const char *value, Names &) {
    return value ? escaped(value) : "null";
  }
};
// As the generic form, / between its parts on every system.
template <> struct Shown<std::filesystem::path> {
  static std::string text(const std::filesystem::path &value, Names &) {
    const std::u8string spelled = value.generic_u8string();
    return "path " +
           escaped({reinterpret_cast<const char *>(spelled.data()), spelled.size()});
  }
};
// A handle the row numbers; null as null.
template <class T> struct Shown<T *> {
  static std::string text(const T *value, Names &names) {
    return value ? std::format("#{}", names.number(value)) : "null";
  }
};
template <class T> struct Shown<std::optional<T>> {
  static std::string text(const std::optional<T> &value, Names &names) {
    return value ? show(*value, names) : "none";
  }
};
template <class A, class B> struct Shown<std::pair<A, B>> {
  static std::string text(const std::pair<A, B> &value, Names &names) {
    return std::format("({}, {})", show(value.first, names), show(value.second, names));
  }
};
template <glm::length_t L, class T, glm::qualifier Q> struct Shown<glm::vec<L, T, Q>> {
  static std::string text(const glm::vec<L, T, Q> &value, Names &) {
    std::string text = "(";
    for (glm::length_t index = 0; index < L; ++index)
      text.append(index ? "," : "").append(number_text(value[index]));
    return text + ")";
  }
};

// Items in brackets; past this many, the first and last few, the count and a hash of
// those between, so a row stays a line and an item changed between still moves it, as
// bench.py keeps a hash of the text it cuts from a long row.
inline constexpr std::size_t shown_items = 12;
template <class Items> std::string list_text(const Items &items, Names &names) {
  const std::size_t count = std::ranges::size(items);
  std::string text = "[";
  std::string between;
  Names between_names; // so the handles shown keep the numbers they had
  std::size_t index = 0;
  for (const auto &item : items) {
    if (count <= shown_items || index < shown_items - 4 || index + 4 >= count)
      text.append(index ? ", " : "").append(show(item, names));
    else
      between.append(show(item, between_names)).push_back('\n');
    if (count > shown_items && index == shown_items - 4)
      text.append(", ...");
    ++index;
  }
  return count <= shown_items ? text + "]"
                              : std::format("{}] ({} items, those between #{:016x})",
                                            text,
                                            count,
                                            hash_of(between));
}
template <class T> struct Shown<std::vector<T>> {
  static std::string text(const std::vector<T> &value, Names &names) {
    return list_text(value, names);
  }
};
template <class T, std::size_t N> struct Shown<std::span<T, N>> {
  static std::string text(std::span<T, N> value, Names &names) {
    return list_text(value, names);
  }
};
template <class T, std::size_t N> struct Shown<std::array<T, N>> {
  static std::string text(const std::array<T, N> &value, Names &names) {
    return list_text(value, names);
  }
};

} // namespace VP::bench
