#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/Labels.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::uint32_t row_room = 64;         // Items shown at most; the rest are cut
constexpr std::uint32_t character_room = 4096; // their labels together
constexpr std::int32_t margin = 3;             // between a column's rows and its frame
constexpr std::int32_t frame_width = 1;
constexpr std::int32_t gap = 2; // between Items side by side, where the ground shows
constexpr std::uint32_t mark_cells = 2;      // a close mark's: a blank, then the mark
constexpr std::string_view mark_text = "x"; // which the font draws, as ASCII
// Behind a column, a frame and the fill inside it, so it reads as raised over what it
// covers. Behind a row, its ground, then a Rect an Item, so each reads as a tab on a bar.
constexpr std::uint32_t frame_and_fill = 2;
constexpr std::uint32_t rect_room = 1 + row_room;

VP_VIEW::Rect grown(const VP_VIEW::Rect &rect, std::int32_t by, std::uint32_t role) {
  return {.offset = rect.offset - by,
          .extent = rect.extent + glm::uvec2(static_cast<std::uint32_t>(2 * by)),
          .role = role};
}

// Lays out the Items it is handed in the Rect its place gives: stacked, a row each, over
// a framed fill, or with axis x side by side, each as wide as its label, over a ground,
// the current one in the content's color, so it joins the content it names; an Item
// with a close command shows a close mark at its end. It hands on where each Item shows,
// and where its mark does, so hit finds the one pressed. Completions and tab strips are
// this part, as menus and popups will be, so none of them places its entries itself.
class List final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string axis = node.param<std::string>("axis");
    if (!axis.empty() && axis != "x" && axis != "y") // the loader names it unset
      throw std::runtime_error(
          std::format("param axis = {}: x, side by side, or y, stacked", axis));
    _side_by_side = axis == "x";
    _items = &node.input<VP_VIEW::Items>("items");
    _place = &node.input<VP_VIEW::Rect>("place");
    _font = &node.input<VP_VIEW::Font>("font");
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", 2 * row_room); // a label, a mark
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
    _shown = &node.output<VP_VIEW::Rects>("shown");
    _shown->reserve(row_room); // so a frame never grows it (CPP10)
    _marks = &node.output<VP_VIEW::Rects>("marks");
    _marks->reserve(row_room);
  }

  void cook(VP::Cook &frame) override {
    const glm::uvec2 cell = _font->cell;
    _shown->clear();
    _marks->clear();
    if (cell.x != 0 && cell.y != 0) {
      if (_side_by_side)
        side_by_side(cell);
      else
        stacked(cell);
    }
    back(frame);
    write_labels(frame, cell);
  }

  // A row each, from the place's top, as many as it holds.
  void stacked(glm::uvec2 cell) {
    const VP_VIEW::Rect &place = *_place;
    const std::uint32_t rows = std::min(
        {static_cast<std::uint32_t>(_items->size()), place.extent.y / cell.y, row_room});
    for (std::uint32_t row = 0; row < rows; ++row) {
      const auto down = static_cast<std::int32_t>(row * cell.y);
      _shown->push_back({.offset = place.offset + glm::ivec2(0, down),
                         .extent = {place.extent.x, cell.y}});
    }
  }

  // From the place's left, each as tall as the place and as wide as its label, and its
  // close mark, with a cell either side; the place's right edge cuts the last, which
  // shows what fits, its mark only when it fits whole.
  void side_by_side(glm::uvec2 cell) {
    const VP_VIEW::Rect &place = *_place;
    if (place.extent.y < cell.y)
      return;
    const auto padding = static_cast<std::int32_t>(2 * cell.x);
    const auto across = static_cast<std::int32_t>(cell.x);
    const std::int32_t right = place.offset.x + static_cast<std::int32_t>(place.extent.x);
    std::int32_t left = place.offset.x;
    for (const VP_VIEW::Item &item : *_items) {
      const std::int32_t room = right - left;
      if (room <= padding || _shown->size() == row_room)
        return;
      const std::size_t cells = item.label.size() + (item.close.empty() ? 0 : mark_cells);
      const auto wide = static_cast<std::int32_t>(cells) * across + padding;
      _shown->push_back(
          {.offset = {left, place.offset.y},
           .extent = {static_cast<std::uint32_t>(std::min(wide, room)), place.extent.y},
           .role = item.current ? VP_VIEW::role("background") : VP_VIEW::role("panel")});
      // The mark's cell is the last before the tab's right padding.
      const std::int32_t mark = left + wide - padding;
      _marks->push_back(item.close.empty() || mark + across > right
                            ? VP_VIEW::Rect{}
                            : VP_VIEW::Rect{.offset = {mark, place.offset.y},
                                            .extent = {cell.x, place.extent.y}});
      left += wide + gap;
    }
  }

  // Nothing shown draws nothing, not even its ground, so a list with no room or no Items
  // costs no draw.
  void back(VP::Cook &frame) {
    const VP_VIEW::Rect &place = *_place;
    if (_shown->empty()) {
      frame.write(_rects, 0);
    } else if (_side_by_side) {
      const std::span<VP_VIEW::Rect> rects = frame.write(_rects, 1 + _shown->size());
      rects[0] = grown(place, 0, VP_VIEW::role("panel"));
      std::ranges::copy(*_shown, rects.begin() + 1);
    } else {
      const std::span<VP_VIEW::Rect> rects = frame.write(_rects, frame_and_fill);
      rects[0] = grown(place, margin + frame_width, VP_VIEW::role("border"));
      rects[1] = grown(place, margin, VP_VIEW::role("panel"));
    }
  }

  // Each label in its Item's Rect: the row itself, or a cell in from either side of a tab
  // and centered top to bottom, and a tab's close mark in its cell, muted. What its room
  // lacks is cut.
  void write_labels(VP::Cook &frame, glm::uvec2 cell) {
    Labels written{frame.write(_labels), frame.write(_characters)};
    for (std::uint32_t at = 0; at < _shown->size(); ++at) {
      VP_VIEW::Rect text = (*_shown)[at];
      if (_side_by_side) {
        text.offset += glm::ivec2(glm::uvec2(cell.x, (text.extent.y - cell.y) / 2));
        text.extent = {text.extent.x - 2 * cell.x, cell.y};
      }
      const std::string_view label = (*_items)[at].label;
      written.add(
          {.offset = text.offset, .extent = text.extent, .role = VP_VIEW::role("text")},
          label.substr(0, text.extent.x / cell.x));
      if (at < _marks->size() && (*_marks)[at].extent.x != 0)
        written.add({.offset = {(*_marks)[at].offset.x, text.offset.y},
                     .extent = {cell.x, cell.y},
                     .role = VP_VIEW::role("muted")},
                    mark_text);
    }
    frame.write(_labels, written.label_count);
    frame.write(_characters, written.character_count); // the used length glyphs draws
  }

  bool _side_by_side = false;
  const VP_VIEW::Items *_items = nullptr;
  const VP_VIEW::Rect *_place = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  VP_VIEW::Rects *_shown = nullptr; // a Rect each Item shown, in the order of the Items
  VP_VIEW::Rects *_marks = nullptr; // and its close mark's, none for no mark
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<List>("List");
}
