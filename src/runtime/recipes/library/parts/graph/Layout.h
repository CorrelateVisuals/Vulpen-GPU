#pragma once

#include "Arranged.h"
#include "Boxes.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: where each box shows, which the
// graph alone works out, but for the boxes a person placed.
namespace {

constexpr std::int32_t pad = 4;         // in pixels, above and below a box's rows
constexpr std::int32_t column_gap = 4;  // in cells, where the curves run
constexpr std::int32_t name_margin = 1; // in cells, either side of a row in its box

// Where the graph alone puts its first box: half a column's gap in from the left and a
// row down, so a home that fits shows the boxes as far in.
glm::ivec2 margin_of(glm::uvec2 cell) {
  return {static_cast<std::int32_t>(column_gap * cell.x) / 2,
          static_cast<std::int32_t>(cell.y)};
}

// Where a box shows at zoom 1, and how large it is with the boxes inside it.
struct Placed {
  // From the top left of the boxes inside the box it is in, below that box's rows, or
  // from the graph's top left for a box at the top.
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  glm::ivec2 at{};         // from the graph's top left
  std::int32_t inside = 0; // from its top, where the boxes inside it start
  std::size_t column = 0;  // among the boxes inside the box it is in
};

// How a connection's end runs: between the boxes holding its two ends inside the box
// both are in, the level, whose columns it orders; back when it runs against them, as
// one that closes a cycle does. One end inside the other's box orders nothing.
struct Leg {
  std::size_t level = no_box;
  std::size_t writer = no_box;
  std::size_t reader = no_box;
  bool back = false;
};

// The graph's layout: each box as large as its rows and the boxes inside it, and those
// in columns, a box a column past every box beside it that it reads from, down each
// column in the manifest's order; a box a person placed where they put it.
class Layout {
public:
  void lay_out(const Boxes &boxes, const Arranged &arranged, glm::uvec2 cell) {
    const std::size_t count = boxes.boxes.size();
    _placed.assign(count, {});
    // Cleared, not made anew, so a layout allocates nothing once they grew (CPP10).
    _children.resize(count + 1); // the top's last
    for (std::vector<std::size_t> &inner : _children)
      inner.clear();
    for (std::size_t box = 0; box < count; ++box)
      children(boxes.boxes[box].parent).push_back(box);
    legs(boxes);
    // Each box after the box it is inside, so going back, the boxes inside one come
    // before it, sized before it is.
    for (std::size_t box = count; box-- > 0;)
      arrange(boxes, box, arranged, cell);
    arrange(boxes, no_box, arranged, cell);
    for (std::size_t box = 0; box < count; ++box) {
      const std::size_t parent = boxes.boxes[box].parent;
      _placed[box].at =
          parent == no_box ? _placed[box].offset
                           : _placed[parent].at + content_of(parent, cell) +
                                 _placed[box].offset;
    }
  }

  std::span<const Placed> placed() const {
    return _placed;
  }
  std::span<const Leg> legs() const {
    return _legs;
  }
  // Where the boxes inside a box start, from its top left.
  glm::ivec2 content_of(std::size_t box, glm::uvec2 cell) const {
    return {name_margin * static_cast<std::int32_t>(cell.x), _placed[box].inside};
  }
private:
  std::vector<std::size_t> &children(std::size_t box) {
    return _children[box == no_box ? _children.size() - 1 : box];
  }

  // For each end, the box both its ends are inside, and the boxes inside that one that
  // hold each end; then which run against the columns, a depth-first walk from each box
  // in turn finding the ends that lead back to a box it is still walking from.
  void legs(const Boxes &boxes) {
    _legs.clear();
    for (const End &end : boxes.ends) {
      Leg leg;
      for (std::size_t writer = end.from; writer != no_box && leg.level == no_box;
           writer = boxes.boxes[writer].parent)
        for (std::size_t reader = end.to; reader != no_box;
             reader = boxes.boxes[reader].parent)
          if (boxes.boxes[writer].parent == boxes.boxes[reader].parent &&
              writer != reader) {
            leg = {.level = boxes.boxes[writer].parent, .writer = writer, .reader = reader};
            break;
          }
      _legs.push_back(leg);
    }
    _walked.assign(boxes.boxes.size(), Walk::not_yet);
    for (std::size_t box = 0; box < boxes.boxes.size(); ++box)
      if (_walked[box] == Walk::not_yet)
        walk(box);
  }

  enum class Walk : std::uint8_t { not_yet, walking, walked };

  void walk(std::size_t box) {
    _walked[box] = Walk::walking;
    for (Leg &leg : _legs)
      if (leg.writer == box) {
        if (_walked[leg.reader] == Walk::walking)
          leg.back = true;
        else if (_walked[leg.reader] == Walk::not_yet)
          walk(leg.reader);
      }
    _walked[box] = Walk::walked;
  }

  // The boxes inside one, in columns or where a person put them, and its own size: as
  // wide as its widest row with a cell either side, and as tall as its rows with pad
  // above and below them, then the boxes inside it, with a cell around them.
  void arrange(const Boxes &boxes,
               std::size_t box,
               const Arranged &arranged,
               glm::uvec2 cell) {
    const std::vector<std::size_t> &inner = children(box);
    glm::ivec2 room{};
    if (!inner.empty())
      room = columns(boxes, box, inner, arranged, cell);
    if (box == no_box)
      return;
    const auto margin = static_cast<std::int32_t>(name_margin * cell.x);
    std::size_t widest = boxes.boxes[box].title.size();
    for (const BoxRow &row : rows_in(boxes, box))
      widest = std::max(widest,
                        row.text.size() + row.written.size() +
                            (row.text.empty() || row.written.empty() ? 0 : 1));
    const auto rows = static_cast<std::int32_t>(1 + boxes.boxes[box].rows);
    // Half a row between its rows and the boxes inside it, and below them.
    const std::int32_t apart = inner.empty() ? 0 : static_cast<std::int32_t>(cell.y) / 2;
    Placed &placed = _placed[box];
    placed.inside = rows * static_cast<std::int32_t>(cell.y) + 2 * pad + apart;
    placed.extent = glm::uvec2(
        std::max(static_cast<std::int32_t>(widest) * static_cast<std::int32_t>(cell.x),
                 room.x) +
            2 * margin,
        placed.inside + (inner.empty() ? 0 : room.y + apart));
  }

  // A box a column past every box beside it that it reads from, each column as wide as
  // its widest box and a gap after it, and its boxes one under the other, a row of text
  // apart; a box a person placed where they put it. Returns how far they reach.
  glm::ivec2 columns(const Boxes &boxes,
                     std::size_t level,
                     const std::vector<std::size_t> &inner,
                     const Arranged &arranged,
                     glm::uvec2 cell) {
    for (std::size_t round = 0; round < inner.size(); ++round) {
      bool moved = false;
      for (const Leg &leg : _legs)
        if (leg.level == level && leg.writer != no_box && !leg.back &&
            _placed[leg.reader].column < _placed[leg.writer].column + 1) {
          _placed[leg.reader].column = _placed[leg.writer].column + 1;
          moved = true;
        }
      if (!moved)
        break;
    }
    std::size_t count = 0;
    for (const std::size_t box : inner)
      count = std::max(count, _placed[box].column + 1);
    const glm::ivec2 start = level == no_box ? margin_of(cell) : glm::ivec2(0);
    _lefts.assign(count, 0);
    _downs.assign(count, start.y);
    for (const std::size_t box : inner)
      _lefts[_placed[box].column] = std::max(
          _lefts[_placed[box].column], static_cast<std::int32_t>(_placed[box].extent.x));
    const auto gap = static_cast<std::int32_t>(column_gap * cell.x);
    std::int32_t left = start.x;
    for (std::int32_t &width : _lefts)
      left += std::exchange(width, left) + gap;
    glm::ivec2 reach{};
    for (const std::size_t box : inner) {
      Placed &placed = _placed[box];
      placed.offset = {_lefts[placed.column], _downs[placed.column]};
      _downs[placed.column] += static_cast<std::int32_t>(placed.extent.y + cell.y);
      // Inside a box, a place stays inside it, as a drag keeps it, though an .ini
      // written before the box held it may say otherwise.
      if (const auto place = arranged.places.find(boxes.boxes[box].name);
          boxes.boxes[box].own && place != arranged.places.end())
        placed.offset =
            level == no_box ? place->second : glm::max(place->second, glm::ivec2(0));
      reach = glm::max(reach, placed.offset + glm::ivec2(placed.extent));
    }
    return reach;
  }

  std::vector<Placed> _placed;
  std::vector<Leg> _legs;
  std::vector<std::vector<std::size_t>> _children; // by box, the top's last
  std::vector<Walk> _walked;
  std::vector<std::int32_t> _lefts; // each column's left; its width while laid out
  std::vector<std::int32_t> _downs; // each column's next row
};

} // namespace
