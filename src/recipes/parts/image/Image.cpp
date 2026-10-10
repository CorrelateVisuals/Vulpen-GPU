#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <span>

namespace {

// Seats the image in the Rect its area gives, as a panel seats any content, and its
// shaders letterbox it there: one instance of its draw, none while the area is empty, so
// an image given no room costs no draw.
class Image final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _area = &node.input<VP_VIEW::Rect>("area");
    _place = node.upload<VP_VIEW::Rect>("place");
  }

  void cook(VP::Cook &frame) override {
    const bool room = _area->extent.x != 0 && _area->extent.y != 0;
    const std::span<VP_VIEW::Rect> place = frame.write(_place, room ? 1 : 0);
    if (room)
      place.front() = *_area;
  }

  const VP_VIEW::Rect *_area = nullptr;
  VP::Upload<VP_VIEW::Rect> _place;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Image>("Image");
}
