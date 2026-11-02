#include "contracts/Font.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/Halves.h"
#include "shared/Params.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>

namespace {

// Cuts a bar off one edge of the Rect its area gives, size text cells across: rows at the
// top or bottom, columns at the left or right, so it follows the font. It hands on the
// bar as strip and what is left as rest, so a strip of tabs and the content under it
// never wait on each other, and neither knows the other's size. A bar has no seam, so at
// a side it seats a panel of a set width; a size of 0 leaves it no room at all. With seam
// = yes a seam's width parts the strip from the rest, kept while the strip has no room,
// where its host draws a divider that a drag hides the bar by and shows it again.
class Bar final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string edge = node.param<std::string>("edge");
    check_word("edge", edge, {"top", "bottom", "left", "right"});
    _along = edge == "left" || edge == "right" ? 0 : 1;
    _far = edge == "bottom" || edge == "right";
    _size = node.param<std::uint32_t>("size");
    const std::string seam = node.param<std::string>("seam");
    check_word("seam", seam, {"yes", "no"});
    _seam = seam == "yes" ? static_cast<std::uint32_t>(seam_width) : 0;
    _area = &node.input<VP_VIEW::Rect>("area");
    _font = &node.input<VP_VIEW::Font>("font");
    _strip = &node.output<VP_VIEW::Rect>("strip");
    _rest = &node.output<VP_VIEW::Rect>("rest");
  }

  void cook(VP::Cook &) override {
    const std::uint32_t whole = _area->extent[_along];
    const std::uint32_t length = bar_length(_size, _font->cell[_along], whole);
    const std::uint32_t parted = std::min(whole - length, _seam);
    *_strip = *_area;
    *_rest = *_area;
    _strip->extent[_along] = length;
    _rest->extent[_along] = whole - length - parted;
    if (_far)
      _strip->offset[_along] += static_cast<std::int32_t>(whole - length);
    else
      _rest->offset[_along] += static_cast<std::int32_t>(length + parted);
  }

  glm::length_t _along = 1; // 0 for x, 1 for y: the axis its size runs along
  bool _far = false;        // at the bottom or the right
  std::uint32_t _size = 0;
  std::uint32_t _seam = 0; // pixels parting the strip from the rest
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP_VIEW::Rect *_strip = nullptr;
  VP_VIEW::Rect *_rest = nullptr;
};

} // namespace

VP_OPERATOR(Bar);
