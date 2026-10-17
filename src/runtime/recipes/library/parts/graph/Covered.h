#pragma once

#include "contracts/Rect.h"
#include "Boxes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: the order the boxes are drawn in,
// and what of a row of text the boxes drawn over it leave showing. glyphs draws every row
// after rects draws every box, so the graph cuts each row at the boxes over it, and a box
// hides the boxes behind it, their text too.
namespace {

constexpr std::size_t piece_room = 16; // pieces a row shows in at most; the rest go

// The boxes inside one, or at the top, each followed by the boxes inside it, so a frame
// stays under what it holds: those raised last over the others, as a press or select
// raises them, and the rest in the order they were built, under those.
template <class Rank>
void add_inside(const Boxes &boxes,
                std::size_t parent,
                const Rank &rank,
                std::vector<std::size_t> &order) {
  std::vector<std::size_t> inner;
  for (std::size_t box = 0; box < boxes.boxes.size(); ++box)
    if (boxes.boxes[box].parent == parent)
      inner.push_back(box);
  std::ranges::sort(inner, {}, rank);
  for (const std::size_t box : inner) {
    order.push_back(box);
    add_inside(boxes, box, rank, order);
  }
}

// The order the boxes are drawn in, the last over the others.
void draw_order(const Boxes &boxes,
                std::span<const std::string> raised,
                std::vector<std::size_t> &order) {
  order.clear();
  add_inside(boxes, no_box, [&](std::size_t box) {
    const auto found = std::ranges::find(raised, boxes.boxes[box].name);
    return std::pair(found == raised.end() ? std::ptrdiff_t{-1} : found - raised.begin(),
                     box);
  }, order);
}

// What of a Rect still shows, in pieces.
struct Pieces {
  std::array<VP_VIEW::Rect, piece_room> rects{};
  std::size_t count = 0;
};

// What of a piece a Rect over it leaves showing: above it, below it, and either side.
void subtract(const VP_VIEW::Rect &piece, const VP_VIEW::Rect &cover, Pieces &left) {
  const glm::ivec2 low = piece.offset;
  const glm::ivec2 high = low + glm::ivec2(piece.extent);
  const glm::ivec2 cover_low = cover.offset;
  const glm::ivec2 cover_high = cover_low + glm::ivec2(cover.extent);
  const auto add = [&](glm::ivec2 from, glm::ivec2 to) {
    if (to.x > from.x && to.y > from.y && left.count < piece_room)
      left.rects[left.count++] = {
          .offset = from, .extent = glm::uvec2(to - from), .role = piece.role};
  };
  if (cover_high.x <= low.x || cover_low.x >= high.x || cover_high.y <= low.y ||
      cover_low.y >= high.y) {
    add(low, high);
    return;
  }
  const std::int32_t top = std::max(low.y, cover_low.y);
  const std::int32_t bottom = std::min(high.y, cover_high.y);
  add(low, {high.x, cover_low.y});
  add({low.x, cover_high.y}, high);
  add({low.x, top}, {cover_low.x, bottom});
  add({cover_high.x, top}, {high.x, bottom});
}

// What of a Rect the Rects over it leave showing, in at most piece_room pieces; a row cut
// into more shows so little that the rest go.
Pieces uncovered(const VP_VIEW::Rect &rect, std::span<const VP_VIEW::Rect> over) {
  Pieces shown;
  shown.rects[shown.count++] = rect;
  for (const VP_VIEW::Rect &cover : over) {
    Pieces left;
    for (const VP_VIEW::Rect &piece : std::span(shown.rects).first(shown.count))
      subtract(piece, cover, left);
    shown = left;
    if (shown.count == 0)
      break;
  }
  return shown;
}

} // namespace
