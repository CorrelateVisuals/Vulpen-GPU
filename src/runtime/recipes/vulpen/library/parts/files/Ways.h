#pragma once

// How a start's ways come joined: the rows of a way in a start, and the ways a .cpp and a
// shader one line makes beside it come joined by, kept apart from the files part's
// commands, so neither file grows past what one reads whole (CPP12).

#include "runtime/Operator.h"
#include "shared/Words.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// "And we know that the Son of God is come, and hath given us an understanding, that we
// may know him that is true, and we are in him that is true, even in his Son Jesus
// Christ. This is the true God, and eternal life." 1 John 5:20

// The ways a .cpp and a shader one line makes beside it come joined by, by their numbers
// in the starts: a .comp reads the C++'s value and hands its results back, [2] and [5];
// a .vert reads the value, [2]. So the node runs joined at once, the least that shows
// C++ and a shader talking.
constexpr std::string_view dispatch_ways = "25";
constexpr std::string_view draw_ways = "2";

// A row of a start's way: commented out, and the comment after its code begins with the
// way's number, or with two, as `// pass.results.at[i] = 0.0; // [2] [5]`. Where its //
// is, where that comment is, and the numbers; none for any other row. Each number is a
// digit in brackets and a space.
constexpr std::string_view comment_mark = "// ";
constexpr std::string_view way_comment = "// [";
constexpr std::size_t way_number_width = 4; // as `[2] `

struct WayRow {
  std::size_t mark = 0;
  std::size_t comment = 0;
  std::string numbers;
};

std::optional<WayRow> way_row(std::string_view row) {
  const std::size_t mark = row.find_first_not_of(' ');
  if (mark == std::string_view::npos || !row.substr(mark).starts_with(comment_mark))
    return std::nullopt;
  WayRow found{.mark = mark,
               .comment = row.find(way_comment, mark + comment_mark.size())};
  if (found.comment == std::string_view::npos)
    return std::nullopt;
  for (std::size_t at = found.comment + comment_mark.size();
       at + 2 < row.size() && row[at] == '[' && row[at + 2] == ']';
       at += way_number_width)
    found.numbers.push_back(row[at + 1]);
  if (found.numbers.empty())
    return std::nullopt;
  return found;
}

// A row commented out, with no number, whose code opens a block, or closes one, as a
// pass block's first and last rows.
bool block_row(std::string_view row, bool opens) {
  const std::size_t mark = row.find_first_not_of(' ');
  if (mark == std::string_view::npos || !row.substr(mark).starts_with(comment_mark) ||
      way_row(row))
    return false;
  const std::string_view code = row.substr(mark + comment_mark.size());
  return opens ? code.ends_with('{') : code.starts_with('}');
}

// A start with the ways given joined: each row of a way whose numbers are all given
// uncommented, its comment kept in its column, three on as the // before its code goes,
// and around them the rows commented out that open and close their block.
std::string joined(std::string_view text, std::string_view ways) {
  std::vector<std::string> rows = rows_of_text(text);
  std::vector<bool> taken(rows.size());
  const std::size_t width = comment_mark.size(); // what uncommenting takes off
  for (std::size_t at = 0; at < rows.size(); ++at)
    if (const std::optional<WayRow> way = way_row(rows[at]);
        way && std::ranges::all_of(way->numbers, [&](char number) {
          return ways.find(number) != std::string_view::npos;
        })) {
      const std::string &row = rows[at];
      rows[at] =
          std::format("{}{}{}{}",
                      row.substr(0, way->mark),
                      row.substr(way->mark + width, way->comment - way->mark - width),
                      std::string(width, ' '),
                      row.substr(way->comment));
      taken[at] = true;
    }
  for (std::size_t open = 0; open < rows.size(); ++open) {
    if (!block_row(rows[open], true))
      continue;
    std::size_t close = open + 1;
    while (close < rows.size() && !block_row(rows[close], false))
      ++close;
    const auto inside = taken.begin() + static_cast<std::ptrdiff_t>(open);
    const auto after = taken.begin() + static_cast<std::ptrdiff_t>(close);
    if (close < rows.size() && std::find(inside, after, true) != after)
      for (const std::size_t at : {open, close})
        rows[at].erase(rows[at].find(comment_mark), width);
  }
  std::string start;
  for (std::size_t at = 0; at < rows.size(); ++at)
    start.append(at == 0 ? "" : "\n").append(rows[at]);
  return start;
}

// The ways a start comes joined by: a .cpp and a .comp or .vert one line makes in the
// same folder, as one node's, read each other at once, with no line to uncomment; none
// beside more C++ than one, whose classes would bind the same names.
std::string_view joined_ways(const std::filesystem::path &file,
                             std::span<const std::filesystem::path> line) {
  const auto made = [&](std::string_view extension) {
    return std::ranges::count_if(line, [&](const std::filesystem::path &path) {
      return path.parent_path() == file.parent_path() && path.extension() == extension;
    });
  };
  if (made(".cpp") != 1)
    return {};
  const std::string extension = file.extension().string();
  if (extension == ".cpp")
    return made(".comp") != 0 ? dispatch_ways : made(".vert") != 0 ? draw_ways : "";
  return extension == ".comp" ? dispatch_ways : extension == ".vert" ? draw_ways : "";
}

// The C++ a shader's start names as its node's: the .cpp the line makes beside it, else
// the first its folder holds, else one of the shader's name, as new would make it.
std::string cpp_beside(VP::FilePort &files,
                       const std::filesystem::path &file,
                       std::span<const std::filesystem::path> line) {
  const auto beside = [&](const std::filesystem::path &path) {
    return path.parent_path() == file.parent_path() && path.extension() == ".cpp";
  };
  if (const auto made = std::ranges::find_if(line, beside); made != line.end())
    return made->filename().string();
  for (const std::string &name : files.list(file.parent_path().string()))
    if (beside(file.parent_path() / name))
      return name;
  return file.stem().string() + ".cpp";
}

} // namespace
