#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/Labels.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t row_room = 64;         // Items shown at most; the rest are cut
constexpr std::uint32_t character_room = 4096; // their labels together
constexpr std::int32_t margin = 3;             // between a column's rows and its frame
constexpr std::int32_t frame_width = 1;
constexpr std::int32_t gap = 2; // between Items side by side, where the ground shows
constexpr std::uint32_t mark_cells = 2;      // a close mark's: a blank, then the mark
constexpr std::string_view mark_text = "x"; // which the font draws, as ASCII
// Cells a tab's label shows at most, the rest cut at its start, so a strip of long names
// shows several; the pointer over one cut shows it whole below the tab.
constexpr std::size_t tab_cells = 24;
constexpr std::int32_t padding_cells = 2; // a tab's, a cell either side of its label
constexpr float wheel_cells = 4.0f;       // a strip scrolls across a step of the wheel
// Behind a column, a frame and the fill inside it, so it reads as raised over what it
// covers, and the current Item's row, so the menu open reads apart. Behind a row, its
// ground, then a Rect an Item, so each reads as a tab on a bar, and a tab's whole label
// in a frame and a fill of its own.
constexpr std::uint32_t frame_and_fill = 2;
constexpr std::uint32_t rect_room = 1 + row_room + frame_and_fill;
// A label and a mark an Item, and a tip.
constexpr std::uint32_t label_room = 2 * row_room + 1;

VP_VIEW::Rect grown(const VP_VIEW::Rect &rect, std::int32_t by, std::uint32_t role) {
  return {.offset = rect.offset - by,
          .extent = rect.extent + glm::uvec2(static_cast<std::uint32_t>(2 * by)),
          .role = role};
}

std::int32_t right_of(const VP_VIEW::Rect &rect) {
  return rect.offset.x + static_cast<std::int32_t>(rect.extent.x);
}

// What of a Rect lies across a strip, as tall as it is; none outside it.
VP_VIEW::Rect within(VP_VIEW::Rect rect, const VP_VIEW::Rect &strip) {
  const std::int32_t from = std::max(rect.offset.x, strip.offset.x);
  const std::int32_t to = std::min(right_of(rect), right_of(strip));
  rect.offset.x = from;
  rect.extent = to > from
                    ? glm::uvec2(static_cast<std::uint32_t>(to - from), rect.extent.y)
                    : glm::uvec2(0);
  return rect;
}

// Lays out the Items it is handed in the Rect its place gives: stacked, a row each, over
// a framed fill, or with axis x side by side, each as wide as its label, up to tab_cells
// of it, the rest cut at its start, over a ground, the current one in the content's
// color, so it joins the content it names, the strip scrolling across as the wheel turns
// over it, with no bar, and to show the current one as it changes, its end cells left
// free for a mark its host draws there, and the pointer over a label cut showing it whole
// below its tab; an Item with a close command shows a close mark at its end; a column
// marks its current Item's row. With drop = below the column drops below its place
// instead, as wide as its widest label and as tall as its Items, inside the window, as a
// menu drops below where it was opened; with drop = right it opens beside its place, its
// first row level with it, as a menu's items open beside its name: right of it, or left
// where the window has no room, its frame on the column's own and its fill over their
// shared border, so the two read as one. It hands on where each Item shows, and where its
// mark does, so hit finds the one pressed, and where the current one does, so what it
// opens shows below it. Tab strips, menus and tips are this part, so none of them places
// its entries itself.
class List final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string axis = node.param<std::string>("axis");
    if (!axis.empty() && axis != "x" && axis != "y") // the loader names it unset
      throw std::runtime_error(
          std::format("param axis = {}: x, side by side, or y, stacked", axis));
    _side_by_side = axis == "x";
    const std::string drop = node.param<std::string>("drop");
    if (drop != "no" && drop != "below" && drop != "right")
      throw std::runtime_error(std::format("param drop = {}: no, below or right", drop));
    _drop = drop == "below" ? Drop::below : drop == "right" ? Drop::right : Drop::no;
    _end = node.param<std::uint32_t>("end");
    _items = &node.input<VP_VIEW::Items>("items");
    _place = &node.input<VP_VIEW::Rect>("place");
    _font = &node.input<VP_VIEW::Font>("font");
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", label_room);
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
    _shown = &node.output<VP_VIEW::Rects>("shown");
    _shown->reserve(row_room); // so a frame never grows it (CPP10)
    _marks = &node.output<VP_VIEW::Rects>("marks");
    _marks->reserve(row_room);
    _current = &node.output<VP_VIEW::Rect>("current");
    _laid.reserve(row_room);
  }

  void cook(VP::Cook &frame) override {
    const glm::uvec2 cell = _font->cell;
    _shown->clear();
    _marks->clear();
    _tip.reset();
    const VP_VIEW::Rect place =
        _drop == Drop::no ? *_place : dropped(cell, frame.resolution());
    if (cell.x != 0 && cell.y != 0) {
      if (_side_by_side)
        side_by_side(frame, place, cell);
      else
        stacked(place, cell);
    }
    *_current = {};
    for (std::size_t at = 0; at < _shown->size(); ++at)
      if ((*_items)[at].current)
        *_current = (*_shown)[at];
    back(frame, place);
    write_labels(frame, cell);
  }

  // Below the place, from its left edge, its frame under the place's bottom edge, as wide
  // as the widest label, or the place; or beside it, its first row level with the place's
  // top, its frame on the last column of the frame of the column the place is a row of.
  // As tall as the Items, moved in to stay inside the window; none while the place is
  // empty, as a menu shut.
  VP_VIEW::Rect dropped(glm::uvec2 cell, glm::uvec2 window) {
    const VP_VIEW::Rect &place = *_place;
    _joined = 0;
    if (place.extent.x == 0 || _items->empty())
      return {};
    std::size_t widest = 0;
    for (const VP_VIEW::Item &item : *_items)
      widest = std::max(widest, item.label.size());
    constexpr std::int32_t edge = margin + frame_width;
    const auto rows = static_cast<std::uint32_t>(
        std::min(_items->size(), static_cast<std::size_t>(row_room)));
    const bool below = _drop == Drop::below;
    const glm::uvec2 extent(
        std::max(static_cast<std::uint32_t>(widest) * cell.x,
                 below ? place.extent.x - std::min(place.extent.x, 2u * edge) : 0u),
        rows * cell.y);
    const glm::ivec2 most = glm::ivec2(window) - glm::ivec2(extent) - edge;
    if (below) {
      const glm::ivec2 from(edge, static_cast<std::int32_t>(place.extent.y) + edge);
      return {.offset = glm::max(glm::min(place.offset + from, most), glm::ivec2(edge)),
              .extent = extent};
    }
    constexpr std::int32_t beside = 2 * edge - frame_width;
    const std::int32_t right =
        place.offset.x + static_cast<std::int32_t>(place.extent.x) + beside;
    _joined = right <= most.x ? 1 : -1;
    const std::int32_t x =
        _joined > 0 ? right
                    : place.offset.x - beside - static_cast<std::int32_t>(extent.x);
    return {.offset = {x, std::clamp(place.offset.y, edge, std::max(most.y, edge))},
            .extent = extent};
  }

  // A row each, from the place's top, as many as it holds.
  void stacked(const VP_VIEW::Rect &place, glm::uvec2 cell) {
    const std::uint32_t rows = std::min(
        {static_cast<std::uint32_t>(_items->size()), place.extent.y / cell.y, row_room});
    for (std::uint32_t row = 0; row < rows; ++row) {
      const auto down = static_cast<std::int32_t>(row * cell.y);
      _shown->push_back({.offset = place.offset + glm::ivec2(0, down),
                         .extent = {place.extent.x, cell.y}});
    }
  }

  // The tabs from the strip's left, as far as it is scrolled, each as tall as the place
  // and as wide as its label, up to tab_cells, and its close mark, with a cell either
  // side; the strip is the place but its end cells. What lies outside the strip is cut,
  // a tab wholly outside it showing nowhere, and its mark only whole. The pointer over a
  // tab whose label is cut shows it whole. An Item with no label is a divider, a line in
  // a cell of its own; Items that toggle, side by side, join with no gap between them.
  void side_by_side(VP::Cook &frame, const VP_VIEW::Rect &place, glm::uvec2 cell) {
    if (place.extent.y < cell.y)
      return;
    const auto across = static_cast<std::int32_t>(cell.x);
    VP_VIEW::Rect strip = place;
    strip.extent.x -= std::min(strip.extent.x, _end * cell.x);
    _laid.clear();
    std::int32_t reach = 0;
    std::optional<std::size_t> current;
    for (std::size_t at = 0; at < _items->size() && _laid.size() < row_room; ++at) {
      const VP_VIEW::Item &item = (*_items)[at];
      if (item.label.empty()) {
        _laid.push_back({.offset = {reach + across / 2, place.offset.y + margin},
                         .extent = {static_cast<std::uint32_t>(frame_width),
                                    place.extent.y -
                                        static_cast<std::uint32_t>(2 * margin)},
                         .role = VP_VIEW::role("border")});
        reach += across + gap;
        continue;
      }
      const std::size_t cells = std::min(item.label.size(), tab_cells) +
                                (item.close.empty() ? 0 : mark_cells);
      const auto wide = (static_cast<std::int32_t>(cells) + padding_cells) * across;
      if (item.current)
        current = _laid.size();
      if (item.toggles && at > 0 && (*_items)[at - 1].toggles)
        reach -= gap;
      _laid.push_back({.offset = {reach, place.offset.y},
                       .extent = {static_cast<std::uint32_t>(wide), place.extent.y},
                       .role = fill_of(item)});
      reach += wide + gap;
    }
    scroll(frame, strip, std::max(reach - gap, 0), current, across);
    const auto shift = strip.offset.x - static_cast<std::int32_t>(std::lround(_pan));
    for (std::size_t at = 0; at < _laid.size(); ++at) {
      VP_VIEW::Rect &tab = _laid[at];
      tab.offset.x += shift;
      _shown->push_back(within(tab, strip));
      // The mark's cell is the last before the tab's right padding.
      const VP_VIEW::Rect mark{
          .offset = {right_of(tab) - padding_cells * across, tab.offset.y},
          .extent = {cell.x, place.extent.y}};
      const VP_VIEW::Item &item = (*_items)[at];
      _marks->push_back(item.close.empty() || within(mark, strip).extent != mark.extent
                            ? VP_VIEW::Rect{}
                            : mark);
      if (item.label.size() > tab_cells && VP_VIEW::contains(_shown->back(), _pointer)) {
        _tip = at;
        _tip_row = tip_row(tab, item.label.size(), cell, place, frame.resolution());
      }
    }
  }

  // A tab's fill: the content's color for the current one, so it joins the content it
  // names, and for one that toggles, the accent's while it is on, so the one on of the
  // two reads apart; or the role an Item names, while it is current.
  static std::uint32_t fill_of(const VP_VIEW::Item &item) {
    if (item.role && item.current)
      return *item.role;
    if (item.toggles && item.current)
      return VP_VIEW::role("accent");
    return item.current && !item.toggles ? VP_VIEW::role("background")
                                         : VP_VIEW::role("panel");
  }

  // A tab's label: in the background's color over the accent of one on, muted on one off
  // of those that toggle, else as text; in the background's color over the role an Item
  // names while it is current, and in that role while not, so it reads as what it lights.
  static std::uint32_t text_of(const VP_VIEW::Item &item) {
    if (item.role)
      return item.current ? VP_VIEW::role("background") : *item.role;
    if (!item.toggles)
      return VP_VIEW::role("text");
    return item.current ? VP_VIEW::role("background") : VP_VIEW::role("muted");
  }

  // Below the tab, as long as its label, from where the label starts, moved in to keep
  // its frame inside the place and the window.
  static VP_VIEW::Rect tip_row(const VP_VIEW::Rect &tab,
                               std::size_t length,
                               glm::uvec2 cell,
                               const VP_VIEW::Rect &place,
                               glm::uvec2 window) {
    constexpr std::int32_t edge = margin + frame_width;
    const auto wide = static_cast<std::int32_t>(length * cell.x);
    const std::int32_t most =
        std::min(right_of(place), static_cast<std::int32_t>(window.x)) - edge - wide;
    const std::int32_t x =
        std::max(std::min(tab.offset.x + static_cast<std::int32_t>(cell.x), most),
                 place.offset.x + edge);
    return {.offset = {x, tab.offset.y + static_cast<std::int32_t>(tab.extent.y) + edge},
            .extent = {static_cast<std::uint32_t>(wide), cell.y}};
  }

  // As the wheel turns over the strip, either way, and as the current tab changes, or how
  // many there are, so it shows whole; never past the first tab or the last.
  void scroll(VP::Cook &frame,
              const VP_VIEW::Rect &strip,
              std::int32_t reach,
              std::optional<std::size_t> current,
              std::int32_t across) {
    for (const VP::Event &event : frame.input().events())
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::wheel && VP_VIEW::contains(strip, _pointer))
        _pan -= (event.turn.x != 0.0f ? event.turn.x : event.turn.y) * wheel_cells *
                static_cast<float>(across);
    const auto room = static_cast<float>(strip.extent.x);
    if (current != _followed || _laid.size() != _followed_count) {
      _followed = current;
      _followed_count = _laid.size();
      if (current) {
        const VP_VIEW::Rect &tab = _laid[*current];
        _pan = std::min(_pan, static_cast<float>(tab.offset.x));
        _pan = std::max(_pan, static_cast<float>(right_of(tab)) - room);
      }
    }
    _pan = std::clamp(_pan, 0.0f, std::max(static_cast<float>(reach) - room, 0.0f));
  }

  // Nothing shown draws nothing, not even its ground, so a list with no room or no Items
  // costs no draw.
  void back(VP::Cook &frame, const VP_VIEW::Rect &place) {
    if (_shown->empty()) {
      frame.write(_rects, 0);
    } else if (_side_by_side) {
      const auto tabs = static_cast<std::uint32_t>(_shown->size());
      const std::span<VP_VIEW::Rect> rects =
          frame.write(_rects, 1 + tabs + (_tip ? frame_and_fill : 0));
      rects[0] = grown(place, 0, VP_VIEW::role("panel"));
      std::ranges::copy(*_shown, rects.begin() + 1);
      if (_tip) {
        rects[1 + tabs] = grown(_tip_row, margin + frame_width, VP_VIEW::role("border"));
        rects[2 + tabs] = grown(_tip_row, margin, VP_VIEW::role("panel"));
      }
    } else {
      const bool marked = _current->extent.x != 0;
      const std::span<VP_VIEW::Rect> rects =
          frame.write(_rects, frame_and_fill + (marked ? 1 : 0));
      rects[0] = grown(place, margin + frame_width, VP_VIEW::role("border"));
      rects[1] = grown(place, margin, VP_VIEW::role("panel"));
      rects[1].extent.x += _joined != 0 ? frame_width : 0;
      rects[1].offset.x -= _joined > 0 ? frame_width : 0;
      if (marked)
        rects[2] = {.offset = _current->offset,
                    .extent = _current->extent,
                    .role = VP_VIEW::role("border")};
    }
  }

  // Each label in its Item's Rect: the row itself, or a tab's, and a tab's close mark in
  // its cell, muted, and the label of the tab the pointer is over whole below it. What
  // its room lacks is cut.
  void write_labels(VP::Cook &frame, glm::uvec2 cell) {
    Labels written{frame.write(_labels), frame.write(_characters)};
    for (std::uint32_t at = 0; at < _shown->size(); ++at) {
      if (!_side_by_side) {
        const VP_VIEW::Rect &row = (*_shown)[at];
        written.add(
            {.offset = row.offset, .extent = row.extent, .role = VP_VIEW::role("text")},
            std::string_view((*_items)[at].label).substr(0, row.extent.x / cell.x));
        continue;
      }
      const std::int32_t middle =
          _laid[at].offset.y + static_cast<std::int32_t>(_laid[at].extent.y - cell.y) / 2;
      tab_label(written, at, middle, cell);
      if (const VP_VIEW::Rect &mark = (*_marks)[at]; mark.extent.x != 0)
        written.add({.offset = {mark.offset.x, middle},
                     .extent = {cell.x, cell.y},
                     .role = VP_VIEW::role("muted")},
                    mark_text);
    }
    if (_tip)
      written.add({.offset = _tip_row.offset,
                    .extent = _tip_row.extent,
                    .role = VP_VIEW::role("text")},
                  (*_items)[*_tip].label);
    frame.write(_labels, written.label_count);
    frame.write(_characters, written.character_count); // the used length glyphs draws
  }

  // A tab's label a cell in from its left, cut at its start past tab_cells, and where the
  // strip cuts its tab, a character at a time.
  void tab_label(Labels &written, std::size_t at, std::int32_t top, glm::uvec2 cell) {
    const VP_VIEW::Rect &shows = (*_shown)[at];
    if (shows.extent.x == 0)
      return;
    fit(_fitted, (*_items)[at].label, tab_cells);
    const auto wide = static_cast<std::int32_t>(cell.x);
    const std::int32_t start = _laid[at].offset.x + wide;
    const std::int32_t from = std::max(start, shows.offset.x);
    const std::int32_t to = std::min(
        {start + static_cast<std::int32_t>(std::min(_fitted.size(), tab_cells)) * wide,
         right_of(shows)});
    if (to <= from)
      return;
    const auto first = static_cast<std::size_t>((from - start) / wide);
    const auto last = std::min(_fitted.size(),
                               static_cast<std::size_t>((to - start + wide - 1) / wide));
    const std::int32_t offset = start + static_cast<std::int32_t>(first) * wide;
    written.add({.offset = {offset, top},
                 .extent = {static_cast<std::uint32_t>(to - offset), cell.y},
                 .role = text_of((*_items)[at]),
                 .cut = {static_cast<std::uint32_t>(from - offset), 0}},
                std::string_view(_fitted).substr(first, last - first));
  }

  // Where a column goes: in its place, or dropped below or right of it, as wide as its
  // labels.
  enum class Drop : std::uint8_t { no, below, right };
  bool _side_by_side = false;
  Drop _drop = Drop::no;
  // The side a column dropped beside its place opened at, its fill joining there: 1
  // right, -1 left, 0 none.
  std::int32_t _joined = 0;
  const VP_VIEW::Items *_items = nullptr;
  const VP_VIEW::Rect *_place = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  VP_VIEW::Rects *_shown = nullptr; // a Rect each Item shown, in the order of the Items
  VP_VIEW::Rects *_marks = nullptr; // and its close mark's, none for no mark
  VP_VIEW::Rect *_current = nullptr; // where the current Item shows; none for none
  // Side by side: the cells a strip leaves free at its end, how far it is scrolled, in
  // pixels, the pointer as the events so far left it, each tab whole before the strip
  // cuts it, the current tab it scrolled to last and how many there were then, and the
  // tab whose whole label shows below it, and where.
  std::uint32_t _end = 0;
  float _pan = 0.0f;
  glm::vec2 _pointer{};
  std::vector<VP_VIEW::Rect> _laid;
  std::optional<std::size_t> _followed;
  std::size_t _followed_count = 0;
  std::optional<std::size_t> _tip;
  VP_VIEW::Rect _tip_row{};
  std::string _fitted; // a tab's label as it shows, kept so a frame allocates none
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<List>("List");
}
