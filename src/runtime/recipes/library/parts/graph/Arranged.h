#pragma once

#include "contracts/Palette.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Ini.h"

#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

// Only Graph.h includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: what the graph keeps in its
// .ini, and how it reads and writes it.
namespace {

constexpr std::string_view kept_folder = ".vulpen"; // which no build or scan reads
constexpr std::string_view kept_file = "graph.ini";
constexpr std::string_view place_key = "place ";
constexpr std::string_view size_key = "size ";
constexpr std::string_view color_key = "color ";
constexpr std::string_view kept_header =
    "# The graph editor's look at this view, which a restart keeps: each box placed by\n"
    "# hand, from the graph's top left at zoom 1, or from the top left of the boxes\n"
    "# inside the box holding it, each box made larger by hand, as wide and as tall in\n"
    "# pixels at zoom 1, and the role a kind of node's boxes are drawn in, as color\n"
    "# dispatch = number: draw, dispatch, code, data, group or view, and a role\n"
    "# theme.ini names. The graph part writes it as boxes move, keeping the colors;\n"
    "# delete it to lay the boxes out from the graph again.\n";

// What a node's files make it, which its box's color can tell: a draw, by a .vert or a
// .frag; a dispatch, by a .comp; code with no shader of its own, C++ or GLSL; files
// with no code, data; and nothing of its own, a group of the nodes inside it. A box of a
// view the view hosts is a view.
enum class Kind : std::uint8_t { draw, dispatch, code, data, group, view };
constexpr auto kind_names = std::to_array<std::string_view>(
    {"draw", "dispatch", "code", "data", "group", "view"});

// What a person arranged, which the graph's .ini keeps through a restart: each box placed
// by hand, by its name, each box made larger by hand, and the role of each kind of node's
// boxes, as theme.ini names the roles; a kind with none is drawn as any box.
struct Arranged {
  std::map<std::string, glm::ivec2, std::less<>> places;
  std::map<std::string, glm::ivec2, std::less<>> sizes;
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

// A line place <box> = x y or size <box> = width height, its box's name and its numbers;
// none for a line that is neither, or a size that is not two numbers above zero.
std::optional<std::pair<std::string, glm::ivec2>> numbered(std::string_view key,
                                                           std::string_view value) {
  const std::vector<float> numbers = numbers_of(value);
  const bool sized = key.starts_with(size_key);
  if ((!sized && !key.starts_with(place_key)) || numbers.size() != 2 ||
      (sized && (numbers[0] <= 0.0f || numbers[1] <= 0.0f)))
    return std::nullopt;
  const std::string_view name = trim(key.substr((sized ? size_key : place_key).size()));
  if (name.empty())
    return std::nullopt;
  return std::pair{std::string(name),
                   glm::ivec2(std::lround(numbers[0]), std::lround(numbers[1]))};
}

// Each line as save writes it, place <box> = x y or size <box> = width height, or as a
// person writes it, color <kind> = <role>; # starts a comment. Throws naming the first
// line that is none, and a pan or a zoom an older graph kept as no longer kept, since
// the graph starts at home (A02).
Arranged arranged_of(std::string_view text) {
  Arranged arranged;
  for (const Row &row : rows_of(text)) {
    const auto pair = key_value(row.text);
    const std::string_view key = pair ? pair->first : row.text;
    const std::string_view value = pair ? pair->second : std::string_view{};
    if (key == "pan" || key == "zoom")
      throw std::runtime_error(
          std::format("{}:{}: {}: the graph keeps no pan or zoom, as it starts at home; "
                      "delete the line",
                      kept_file,
                      row.line,
                      row.text));
    if (key.starts_with(color_key)) {
      try {
        color_line(arranged, trim(key.substr(color_key.size())), value);
      } catch (const std::runtime_error &mistake) {
        throw std::runtime_error(
            std::format("{}:{}: {}", kept_file, row.line, mistake.what()));
      }
      continue;
    }
    const auto line = numbered(key, value);
    if (!line)
      throw std::runtime_error(
          std::format("{}:{}: {} is no place <box> = x y, size <box> = width height, "
                      "nor color <kind> = <role>",
                      kept_file,
                      row.line,
                      row.text));
    (key.starts_with(size_key) ? arranged.sizes : arranged.places)
        .insert_or_assign(line->first, line->second);
  }
  return arranged;
}

// As arranged_of reads it: the header, each kind's color, then each box placed, then each
// box made larger.
std::string text_of(const Arranged &arranged) {
  std::string text(kept_header);
  for (std::size_t kind = 0; kind < kind_names.size(); ++kind)
    if (const std::optional<std::uint32_t> role = arranged.colors[kind])
      text += std::format(
          "{}{} = {}\n", color_key, kind_names[kind], VP_VIEW::role_names[*role]);
  for (const auto &[box, place] : arranged.places)
    text += std::format("{}{} = {} {}\n", place_key, box, place.x, place.y);
  for (const auto &[box, size] : arranged.sizes)
    text += std::format("{}{} = {} {}\n", size_key, box, size.x, size.y);
  return text;
}

} // namespace
