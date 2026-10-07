#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

constexpr std::uint32_t row_room = 64;         // rows shown at most; the rest are cut
constexpr std::uint32_t character_room = 4096; // the rows' labels together
constexpr std::int32_t margin = 3;             // between the rows and the frame
constexpr std::int32_t frame_width = 1;
// The frame and the fill inside it, behind the rows, so a list reads as raised over
// what it covers.
constexpr std::uint32_t backing = 2;

VP_VIEW::Rect grown(const VP_VIEW::Rect &rect, std::int32_t by, std::uint32_t role) {
  return {.offset = rect.offset - by,
          .extent = rect.extent + glm::uvec2(static_cast<std::uint32_t>(2 * by)),
          .role = role};
}

// Lays out the Items it is handed as a column of rows, a row each, in the Rect its place
// gives, over a framed fill. A completion list is this part, as menus and popups will
// be, so none of them places its rows itself.
class List final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _items = &node.input<VP_VIEW::Items>("items");
    _place = &node.input<VP_VIEW::Rect>("place");
    _font = &node.input<VP_VIEW::Font>("font");
    _rects = node.upload<VP_VIEW::Rect>("rects", backing);
    _labels = node.upload<VP_VIEW::Label>("labels", row_room);
    _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  }
  void cook(VP::Cook &frame) override {
    const VP_VIEW::Rect &place = *_place;
    const glm::uvec2 cell = _font->cell;
    const std::uint32_t rows =
        cell.y == 0 ? 0
                    : std::min({static_cast<std::uint32_t>(_items->size()),
                                place.extent.y / cell.y,
                                row_room});
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects, rows == 0 ? 0 : backing);
    if (rows != 0) {
      rects[0] = grown(place, margin + frame_width, VP_VIEW::role("border"));
      rects[1] = grown(place, margin, VP_VIEW::role("panel"));
    }
    const std::span<VP_VIEW::Label> labels = frame.write(_labels, rows);
    const std::span<VP_VIEW::Character> room = frame.write(_characters);
    const std::size_t columns = cell.x == 0 ? 0 : place.extent.x / cell.x;
    std::uint32_t next = 0;
    for (std::uint32_t row = 0; row < rows; ++row) {
      const std::string_view label = (*_items)[row].label;
      const auto shown = static_cast<std::uint32_t>(
          std::min({label.size(), columns, std::size_t{character_room - next}}));
      for (std::uint32_t at = 0; at < shown; ++at)
        room[next + at] = {.code = static_cast<unsigned char>(label[at]), .label = row};
      labels[row] = {.offset = place.offset +
                               glm::ivec2(0, static_cast<std::int32_t>(row * cell.y)),
                     .extent = {place.extent.x, cell.y},
                     .role = VP_VIEW::role("text"),
                     .first = next,
                     .count = shown};
      next += shown;
    }
    frame.write(_characters, next); // the used length, which glyphs draws
  }

  const VP_VIEW::Items *_items = nullptr;
  const VP_VIEW::Rect *_place = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<List>("List");
}
