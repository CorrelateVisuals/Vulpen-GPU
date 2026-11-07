#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
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

// Where control with backspace deletes back to from a column, so the editor and the
// terminal delete alike (RV09): past the blanks before it, then the other bytes, then the
// word before them, so a path, a dotted name or a link goes a part at a time, a folder
// with its slash, and blanks with the word before them.
inline std::size_t word_before(std::string_view row, std::size_t column) {
  column = std::min(column, row.size());
  for (const Run run : {Run::blank, Run::other, Run::word})
    while (column > 0 && run_of(row[column - 1]) == run)
      --column;
  return column;
}

// Where control with delete deletes on to from a column, as word_before goes back, the
// other way: past the blanks after it, then the other bytes, then the word after them.
inline std::size_t word_after(std::string_view row, std::size_t column) {
  column = std::min(column, row.size());
  for (const Run run : {Run::blank, Run::other, Run::word})
    while (column < row.size() && run_of(row[column]) == run)
      ++column;
  return column;
}

// The C++ class a name gives: command-line gives CommandLine, as a file new starts names
// its class by its stem, so the parts that name one name it alike (RV09).
inline std::string class_of(std::string_view name) {
  std::string type;
  bool upper = true;
  for (const char letter : name) {
    if (letter == '-') {
      upper = true;
      continue;
    }
    type.push_back(upper && letter >= 'a' && letter <= 'z'
                       ? static_cast<char>(letter - 'a' + 'A')
                       : letter);
    upper = false;
  }
  return type;
}

// The stem a copy's name is numbered from and the first number after it: the name's
// trailing digits counted on, as a copy of wave2 is wave3, else 2, as one of wave is
// wave2.
inline std::pair<std::string_view, std::size_t> numbered(std::string_view name) {
  const std::size_t stem = name.find_last_not_of("0123456789") + 1;
  std::size_t number = 0;
  if (stem == 0 || stem == name.size() ||
      std::from_chars(name.data() + stem, name.data() + name.size(), number).ec !=
          std::errc{})
    return {name, 2};
  return {name.substr(0, stem), number + 1};
}

} // namespace
