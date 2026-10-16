#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// The blanks between a row's words and a value's numbers.
constexpr std::string_view ini_blanks = " \t\r";

// A row of an .ini that holds more than blanks and a comment: its line, counted from 1,
// as a mistake names it, and its text, the comment cut and the blanks trimmed.
struct Row {
  std::size_t line = 0;
  std::string_view text;
};

// Without the blanks at its ends.
inline std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(ini_blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(ini_blanks) - first + 1);
}

// How every part's .ini reads, the palette's theme, the keys' keymap and the files the
// font and the graph keep, so one rule holds for all of them (RV09): a row a line, # and
// what follows a comment, and a row of nothing more left out. The part hands it the
// text it read, and says itself what a row means.
inline std::vector<Row> rows_of(std::string_view text) {
  std::vector<Row> rows;
  for (std::size_t line = 1; !text.empty(); ++line) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view row = trim(text.substr(0, std::min(text.find('#'), end)));
    text.remove_prefix(std::min(end + 1, text.size()));
    if (!row.empty())
      rows.push_back({line, row});
  }
  return rows;
}

// A row's key and its value, trimmed, either side of its first =; none without one.
inline std::optional<std::pair<std::string_view, std::string_view>>
key_value(std::string_view row) {
  const std::size_t equals = row.find('=');
  if (equals == std::string_view::npos)
    return std::nullopt;
  return std::pair{trim(row.substr(0, equals)), trim(row.substr(equals + 1))};
}

// The numbers a value holds, between blanks; none when a word is no number.
inline std::vector<float> numbers_of(std::string_view text) {
  std::vector<float> numbers;
  for (text = trim(text); !text.empty(); text = trim(text)) {
    const std::size_t length = std::min(text.find_first_of(ini_blanks), text.size());
    float number = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + length, number);
    if (error != std::errc{} || end != text.data() + length)
      return {};
    numbers.push_back(number);
    text.remove_prefix(length);
  }
  return numbers;
}

} // namespace
