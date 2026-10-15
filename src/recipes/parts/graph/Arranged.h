#pragma once

#include "contracts/Palette.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Only Graph.h includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: what the graph keeps in its
// .ini, and how it reads and writes it.
namespace {

constexpr std::string_view kept_folder = ".vulpen"; // which no build or scan reads
constexpr std::string_view kept_file = "graph.ini";
constexpr std::string_view place_key = "place ";
constexpr std::string_view color_key = "color ";
constexpr std::string_view blanks = " \t\r";
constexpr std::string_view kept_header =
    "# The graph editor's look at this view, which a restart keeps: each box placed by\n"
    "# hand, from the graph's top left at zoom 1, and the role a kind of node's boxes\n"
    "# are drawn in, as color dispatch = number: draw, dispatch, code, data or group,\n"
    "# and a role theme.ini names. The graph part writes it as boxes move, keeping the\n"
    "# colors; delete it to lay the boxes out from the graph again.\n";

// What a node's files make it, which its box's color can tell: a draw, by a .vert or a
// .frag; a dispatch, by a .comp; code with no shader of its own, C++ or GLSL; files
// with no code, data; and nothing of its own, a group of the nodes inside it.
enum class Kind : std::uint8_t { draw, dispatch, code, data, group };
constexpr auto kind_names =
    std::to_array<std::string_view>({"draw", "dispatch", "code", "data", "group"});

// What a person arranged, which the graph's .ini keeps through a restart: each box placed
// by hand, by its node's name, and the role of each kind of node's boxes, as theme.ini
// names the roles; a kind with none is drawn as any box.
struct Arranged {
  std::map<std::string, glm::ivec2, std::less<>> places;
  std::array<std::optional<std::uint32_t>, kind_names.size()> colors{};
};

// What its files make a node, the first that fits: a draw, a dispatch, code, data, or a
// group with no files of its own.
Kind kind_of(const VP::Node &node) {
  bool draw = false;
  bool dispatch = false;
  bool code = false;
  for (const std::string &file : node.files) {
    const std::string_view ending =
        std::string_view(file).substr(std::min(file.rfind('.'), file.size()));
    draw = draw || ending == ".vert" || ending == ".frag";
    dispatch = dispatch || ending == ".comp";
    code = code || ending == ".cpp" || ending == ".h" || ending == ".glsl";
  }
  if (draw || dispatch)
    return draw ? Kind::draw : Kind::dispatch;
  if (code)
    return Kind::code;
  return node.files.empty() ? Kind::group : Kind::data;
}

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The numbers a value holds, between blanks; none when a word is no number.
std::vector<float> numbers_of(std::string_view text) {
  std::vector<float> numbers;
  for (text = trim(text); !text.empty(); text = trim(text)) {
    const std::size_t length = std::min(text.find_first_of(blanks), text.size());
    float number = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + length, number);
    if (error != std::errc{} || end != text.data() + length)
      return {};
    numbers.push_back(number);
    text.remove_prefix(length);
  }
  return numbers;
}

// The words a list gives, a comma between each, for a mistake to name.
template <std::size_t Count>
std::string listed(const std::array<std::string_view, Count> &words) {
  std::string text;
  for (const std::string_view word : words)
    text.append(text.empty() ? "" : ", ").append(word);
  return text;
}

// A line color <kind> = <role>, the role of that kind of node's boxes. Throws naming a
// kind or a role there is not, or a kind given twice (A02).
void color_line(Arranged &arranged, std::string_view kind, std::string_view role) {
  const auto found = std::ranges::find(kind_names, kind);
  if (found == kind_names.end())
    throw std::runtime_error(
        std::format("no kind is named {}; the kinds are {}", kind, listed(kind_names)));
  const auto named = std::ranges::find(VP_VIEW::role_names, role);
  if (named == VP_VIEW::role_names.end())
    throw std::runtime_error(std::format(
        "no role is named {}; the roles are {}", role, listed(VP_VIEW::role_names)));
  std::optional<std::uint32_t> &color =
      arranged.colors[static_cast<std::size_t>(found - kind_names.begin())];
  if (color)
    throw std::runtime_error(std::format("color {} is given twice", kind));
  color = static_cast<std::uint32_t>(named - VP_VIEW::role_names.begin());
}

// Each line as save writes it, place <node> = x y, or as a person writes it, color
// <kind> = <role>; # starts a comment. Throws naming the first line that is none, and a
// pan or a zoom an older graph kept as no longer kept, since the graph starts at home
// (A02).
Arranged arranged_of(std::string_view text) {
  Arranged arranged;
  for (std::size_t line = 1; !text.empty(); ++line) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view row = trim(text.substr(0, std::min(text.find('#'), end)));
    text.remove_prefix(std::min(end + 1, text.size()));
    if (row.empty())
      continue;
    const std::size_t equals = std::min(row.find('='), row.size());
    const std::string_view key = trim(row.substr(0, equals));
    const std::vector<float> numbers =
        numbers_of(row.substr(std::min(equals + 1, row.size())));
    if (key == "pan" || key == "zoom")
      throw std::runtime_error(
          std::format("{}:{}: {}: the graph keeps no pan or zoom, as it starts at home; "
                      "delete the line",
                      kept_file,
                      line,
                      row));
    if (key.starts_with(color_key)) {
      try {
        color_line(arranged,
                   trim(key.substr(color_key.size())),
                   trim(row.substr(std::min(equals + 1, row.size()))));
      } catch (const std::runtime_error &mistake) {
        throw std::runtime_error(
            std::format("{}:{}: {}", kept_file, line, mistake.what()));
      }
      continue;
    }
    if (!key.starts_with(place_key) || numbers.size() != 2 ||
        trim(key.substr(place_key.size())).empty())
      throw std::runtime_error(std::format(
          "{}:{}: {} is no place <node> = x y, nor color <kind> = <role>",
          kept_file,
          line,
          row));
    arranged.places.insert_or_assign(
        std::string(trim(key.substr(place_key.size()))),
        glm::ivec2(std::lround(numbers[0]), std::lround(numbers[1])));
  }
  return arranged;
}

// As arranged_of reads it: the header, each kind's color, then each box placed.
std::string text_of(const Arranged &arranged) {
  std::string text(kept_header);
  for (std::size_t kind = 0; kind < kind_names.size(); ++kind)
    if (const std::optional<std::uint32_t> role = arranged.colors[kind])
      text += std::format(
          "{}{} = {}\n", color_key, kind_names[kind], VP_VIEW::role_names[*role]);
  for (const auto &[node, place] : arranged.places)
    text += std::format("{}{} = {} {}\n", place_key, node, place.x, place.y);
  return text;
}

} // namespace
