#pragma once

#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Ini.h"

#include <algorithm>
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
constexpr std::string_view color_key = "color "; // as an older graph kept a kind's color
constexpr std::string_view kept_header =
    "# The graph editor's look at this view, which a restart keeps: each box placed by\n"
    "# hand, from the graph's top left at zoom 1, or from the top left of the boxes\n"
    "# inside the box holding it, and each box made larger by hand, as wide and as tall\n"
    "# in pixels at zoom 1. The graph part writes it as boxes move; delete it to lay the\n"
    "# boxes out from the graph again. What a box is colors it, as theme.ini says.\n";

// What a person arranged, which the graph's .ini keeps through a restart: each box placed
// by hand, by its name, and each box made larger by hand.
struct Arranged {
  std::map<std::string, glm::ivec2, std::less<>> places;
  std::map<std::string, glm::ivec2, std::less<>> sizes;
};

// The changes control with z takes back, a drag, a pull or an arrow's step each, as far
// back as undo_room of them, so memory stays bounded (A03).
constexpr std::size_t undo_room = 100;

// What was arranged before a change, or after one taken back, with how many lines undo
// could take back then (CommandPort::done), so control with z takes back whichever of
// the boxes' change and a line came last.
struct Placing {
  Arranged arranged;
  std::size_t lines = 0;
};

// What was arranged before each change, and after each change taken back, which control
// with shift and z does again, the newest last; one an .ini.
struct History {
  std::vector<Placing> undone;
  std::vector<Placing> redone;
};

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

// Each line as save writes it, place <box> = x y or size <box> = width height; # starts
// a comment. Throws naming the first line that is none, a pan or a zoom an older graph
// kept as no longer kept, since the graph starts at home, and a color an older graph
// kept, as the theme now colors every box (A02).
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
    if (key.starts_with(color_key))
      throw std::runtime_error(
          std::format("{}:{}: {}: what a box is colors it, as theme.ini names a role "
                      "for each kind of box; delete the line",
                      kept_file,
                      row.line,
                      row.text));
    const auto line = numbered(key, value);
    if (!line)
      throw std::runtime_error(
          std::format("{}:{}: {} is no place <box> = x y nor size <box> = width height",
                      kept_file,
                      row.line,
                      row.text));
    (key.starts_with(size_key) ? arranged.sizes : arranged.places)
        .insert_or_assign(line->first, line->second);
  }
  return arranged;
}

// As arranged_of reads it: the header, then each box placed, then each box made larger.
std::string text_of(const Arranged &arranged) {
  std::string text(kept_header);
  for (const auto &[box, place] : arranged.places)
    text += std::format("{}{} = {} {}\n", place_key, box, place.x, place.y);
  for (const auto &[box, size] : arranged.sizes)
    text += std::format("{}{} = {} {}\n", size_key, box, size.x, size.y);
  return text;
}

} // namespace
