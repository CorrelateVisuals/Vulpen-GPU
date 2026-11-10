#pragma once

// How the menus part reads menu.ini: a line's words, the columns they take and the
// placeholder a column lists, kept apart from what the menus do with them, so neither
// file grows past what one reads whole (CPP12).

#include "runtime/Operator.h"
#include "shared/Ini.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view menu_file = "menu.ini";
// Where the name of what a menu opened for goes in a line; first, with a colon, it names
// the view the line addresses, as a typed line does. {/} is the folder of what was
// pressed, so a line makes something inside a node or beside a file; {1} and {2} name the
// first and the second of two pressed, {node} a port's node, as node.port names it,
// {view} the view lines go to, and {image} a port pressed that is an image its node
// writes, so a line naming it shows on such a port alone.
constexpr std::string_view subject = "{}";
constexpr std::string_view address_mark = "{}:";
constexpr std::string_view folder_mark = "{/}";
constexpr std::string_view first_mark = "{1}";
constexpr std::string_view second_mark = "{2}";
constexpr std::string_view node_mark = "{node}";
constexpr std::string_view view_mark = "{view}";
constexpr std::string_view image_mark = "{image}";
constexpr std::array<std::string_view, 7> marks{
    subject, folder_mark, first_mark, second_mark, node_mark, view_mark, image_mark};
// The words before what a menu opens for, and before the words the pointer came onto or
// pressed, as the hit part sends them.
constexpr std::string_view for_word = "for";
constexpr std::string_view point_word = "point";
constexpr std::string_view press_word = "press";
// The columns the menu component lays out, a word each.
constexpr std::size_t column_count = 3;
// Placeholders that stand for a few known words, which a column lists as the terminal's
// complete answers them; the person types any other, as a new name.
constexpr std::array<std::string_view, 4> completed_kinds{"<recipe>", "<node>", "<view>",
                                                          "<connection>"};
// And those the menus find themselves, from the view lines go to and its ports, for what
// was pressed: a line connecting a loose port to one alike, which for one node takes two
// columns, its port and the other end; a connection joining what was pressed; a value a
// param may set; a file of what was pressed; and a file new starts with code that changes
// what a node runs, which it lacks: its C++, beside C++ alone a shader, beside a lone
// .vert or .frag the other half of its draw, or a stage between a draw's two.
constexpr std::string_view link_kind = "<link>";
constexpr std::string_view joined_kind = "<joined>";
constexpr std::string_view key_kind = "<key>";
constexpr std::string_view path_kind = "<path>";
constexpr std::string_view start_kind = "<start>";
constexpr std::array<std::string_view, 5> found_kinds{
    link_kind, joined_kind, key_kind, path_kind, start_kind};
// Between words, and between the lines an answer holds, a word each.
constexpr std::string_view blanks = " \t\r\n";

std::vector<std::string> words_of(std::string_view text) {
  std::vector<std::string> words;
  for (std::size_t at = text.find_first_not_of(blanks); at != std::string_view::npos;) {
    const std::size_t end = std::min(text.find_first_of(blanks, at), text.size());
    words.emplace_back(text.substr(at, end - at));
    at = text.find_first_not_of(blanks, end);
  }
  return words;
}

std::string joined(std::span<const std::string> words) {
  std::string text;
  for (const std::string &word : words)
    text.append(text.empty() ? "" : " ").append(word);
  return text;
}

bool one_of(std::span<const std::string_view> kinds, std::string_view kind) {
  return std::ranges::find(kinds, kind) != kinds.end();
}

// A word that holds only marks, as {}, which a column never shows.
bool only_marks(std::string word) {
  for (const std::string_view mark : marks)
    for (std::size_t at = word.find(mark); at != std::string::npos; at = word.find(mark))
      word.erase(at, mark.size());
  return word.empty();
}

// A menu.ini line: the section it is under, a kind of thing pressed, its words as typed,
// and how many of them are a column each, those past {}: before the first that holds a
// mark or a placeholder; and the word holding the placeholder a column lists after them,
// past those that hold only marks, as <key> past {}, with what comes before it in its
// word, as {/}/ before <path>.
struct Line {
  std::string section;
  std::vector<std::string> words;
  std::size_t first = 0; // 1 past {}:
  std::size_t path = 0;
  std::size_t listed = 0; // 0 for none, as a line's first word is never one
  std::string kind;
  std::string prefix;

  std::span<const std::string> columns() const {
    return std::span(words).subspan(first, path);
  }
  bool lists() const {
    return listed != 0;
  }
  bool names_image() const {
    return std::ranges::any_of(words, [](const std::string &word) {
      return word.find(image_mark) != std::string::npos;
    });
  }
  bool names_subject() const {
    return std::ranges::any_of(words, [](const std::string &word) {
      return word.find('{') != std::string::npos;
    });
  }
  // A line for two or more pressed at once, which runs once for them all: it names the
  // first and the second, or lists what joins them.
  bool joins() const {
    return kind == link_kind || kind == joined_kind ||
           std::ranges::any_of(words, [](const std::string &word) {
             return word.find(first_mark) != std::string::npos ||
                    word.find(second_mark) != std::string::npos;
           });
  }
  // A word the person types, which a placeholder no column lists stands for.
  bool wants() const {
    for (std::size_t at = 0; at < words.size(); ++at)
      if (at != listed && words[at].find('<') != std::string::npos)
        return true;
    return false;
  }
};

// A line a command registered now takes, past its {}:, and no more words a column than
// the menu shows, counting those a placeholder it lists takes (A02).
Line line_of(std::string_view text,
             std::string section,
             std::span<const VP::Usage> usages) {
  Line line{.section = std::move(section), .words = words_of(text)};
  line.first = line.words.front() == address_mark ? 1 : 0;
  const std::span<const std::string> sent = std::span(line.words).subspan(line.first);
  if (sent.empty() || !registered(joined(sent), usages))
    throw std::runtime_error(std::format("no command takes {}", text));
  while (line.first + line.path < line.words.size() &&
         line.words[line.first + line.path].find_first_of("{<") == std::string::npos)
    ++line.path;
  for (std::size_t at = line.first + line.path; at < line.words.size(); ++at) {
    const std::string &word = line.words[at];
    if (only_marks(word))
      continue;
    const std::size_t open = word.find('<');
    const std::string kind = open == std::string::npos
                                 ? std::string()
                                 : word.substr(open, word.find('>') - open + 1);
    if (one_of(completed_kinds, kind) || one_of(found_kinds, kind)) {
      line.listed = at;
      line.kind = kind;
      line.prefix = word.substr(0, open);
    }
    break;
  }
  const std::size_t listing = !line.lists() ? 0 : line.kind == link_kind ? 2 : 1;
  if (line.path + listing > column_count)
    throw std::runtime_error(
        std::format("{} takes more columns than the menu's {}", text, column_count));
  return line;
}

// Each line under its section, in the order they come, so a menu shows them so.
std::vector<Line> lines_of(std::string_view text, std::span<const VP::Usage> usages) {
  std::vector<Line> lines;
  std::string section;
  for (const Row &row : rows_of(text)) {
    try {
      if (row.text.starts_with('[')) {
        section = row.text.ends_with(']')
                      ? std::string(trim(row.text.substr(1, row.text.size() - 2)))
                      : std::string();
        if (section.empty())
          throw std::runtime_error("a section is a kind of thing pressed in brackets, as "
                                   "[node]");
        if (section == point_word || section == press_word || section == for_word)
          throw std::runtime_error(
              std::format("[{}] is a word the menu command takes itself", section));
        continue;
      }
      if (section.empty())
        throw std::runtime_error("a line comes under a section, as [node]");
      Line line = line_of(row.text, section, usages);
      if (std::ranges::any_of(lines, [&](const Line &given) {
            return given.section == line.section && given.words == line.words;
          }))
        throw std::runtime_error(std::format("[{}] gives {} twice", section, row.text));
      lines.push_back(std::move(line));
    } catch (const std::runtime_error &mistake) {
      throw std::runtime_error(
          std::format("{}:{}: {}", menu_file, row.line, mistake.what()));
    }
  }
  return lines;
}

} // namespace
