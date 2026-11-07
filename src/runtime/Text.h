#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The small text helpers the engine's files share, each written once (C02, C06): what
// manifests, kept files and lines are read with, and what an error lists names with. At
// the bottom of the include map, it includes nothing of ours.
namespace VP {

// What parts the words of a line, the \r a Windows file ends each line with too.
inline constexpr std::string_view blanks = " \t\r";

// The text without the blanks it starts and ends with.
inline std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The whole text as a number of the type; none where it holds anything more, or a number
// the type cannot hold.
template <class T> std::optional<T> number_in(std::string_view text) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

// Names one after another, as an error lists them, or words as the log keeps them.
template <class Names>
std::string joined(const Names &names, std::string_view between = ", ") {
  std::string text;
  for (const std::string_view name : names)
    text.append(text.empty() ? "" : between).append(name);
  return text;
}

// A text's lines that hold anything, without the \r a Windows file ends each with, as
// views into the text.
inline std::vector<std::string_view> lines_in(std::string_view text) {
  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::size_t stop = end > at && text[end - 1] == '\r' ? end - 1 : end;
    if (stop > at)
      lines.push_back(text.substr(at, stop - at));
    at = end + 1;
  }
  return lines;
}

} // namespace VP
