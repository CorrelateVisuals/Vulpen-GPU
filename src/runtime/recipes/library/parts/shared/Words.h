#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace {

// UTF-8 (RFC 3629): each byte of a character past ASCII is at least this.
constexpr unsigned char past_ascii = 0x80;

// The kinds of byte a run holds one of.
enum class Run : unsigned char { word, blank, other };

inline Run run_of(char byte) {
  const auto code = static_cast<unsigned char>(byte);
  if (code == ' ' || code == '\t')
    return Run::blank;
  const bool letter = (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z');
  const bool digit = code >= '0' && code <= '9';
  return letter || digit || code == '_' || code >= past_ascii ? Run::word : Run::other;
}

// What a double click selects in a row, so the editor and the terminal select alike
// (RV09): the run around the byte pressed of one kind, letters, digits and _, blanks, or
// any other byte, so a press on a name takes the name and one on an operator the
// operator. A byte past ASCII counts as a letter, so a run never splits a character. Past
// the row's end, the run that ends it; none in an empty row.
inline std::pair<std::size_t, std::size_t> word_at(std::string_view row,
                                                   std::size_t column) {
  if (row.empty())
    return {0, 0};
  column = std::min(column, row.size() - 1);
  const Run pressed = run_of(row[column]);
  std::size_t from = column;
  std::size_t to = column + 1;
  while (from > 0 && run_of(row[from - 1]) == pressed)
    --from;
  while (to < row.size() && run_of(row[to]) == pressed)
    ++to;
  return {from, to};
}

} // namespace
