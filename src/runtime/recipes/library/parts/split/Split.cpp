#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/Halves.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr std::uint32_t backing = 2; // the ground under both Rects, then the seam
// A ratio keeps the three decimals param set writes, while dragged too, so letting go
// moves nothing.
constexpr float ratio_steps = 1000.0f;

// Divides the Rect its area gives in two along its axis, the first taking its ratio of
// the room the seam between them leaves. A press on the seam grabs it, both Rects follow
// the pointer, and letting go sends param set, so the log keeps one line a drag and a
// replay puts the seam back (V08). A tree of splits, each dividing a Rect another gave,
// lays out a dock.
class Split final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string axis = node.param<std::string>("axis");
    if (!axis.empty() && axis != "x" && axis != "y") // the loader names it unset
      throw std::runtime_error(
          std::format("param axis = {}: x, side by side, or y, stacked", axis));
    _along = axis == "y" ? 1 : 0;
    _ratio = node.param<float>("ratio");
    if (!(_ratio >= 0.0f && _ratio <= 1.0f)) // NaN too
      throw std::runtime_error(
          std::format("param ratio = {}: a share from 0 to 1", _ratio));
    _name = node.name();
    _view = node.view_name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _first = &node.output<VP_VIEW::Rect>("first");
    _second = &node.output<VP_VIEW::Rect>("second");
    _rects = node.upload<VP_VIEW::Rect>("rects", backing);
  }

  void cook(VP::Cook &frame) override {
    for (const VP::Event &event : frame.input().events())
      follow(frame, event);
    const Halves split = halves(*_area, _along, _ratio);
    *_first = split.first;
    *_second = split.second;
    const VP_VIEW::Rect &seam = split.seam;
    const bool lit = _dragging || VP_VIEW::contains(seam, _pointer);
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects, backing);
    rects[0] = {.offset = _area->offset,
                .extent = _area->extent,
                .role = VP_VIEW::role("background")};
    rects[1] = {.offset = seam.offset,
                .extent = seam.extent,
                .role = lit ? VP_VIEW::role("accent") : VP_VIEW::role("border")};
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void follow(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      if (_dragging)
        _ratio = ratio_at(_pointer[_along]);
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      if (event.down && VP_VIEW::contains(halves(*_area, _along, _ratio).seam, _pointer))
        _dragging = true;
      else if (!event.down && std::exchange(_dragging, false))
        frame.commands().send(
            std::format("{}: param set {} ratio {:.3f}", _view, _name, _ratio));
    }
  }

  // The ratio that puts the seam's middle at a point along the axis.
  float ratio_at(float at) const {
    const Halves split = halves(*_area, _along, _ratio);
    const auto room = static_cast<std::int32_t>(split.first.extent[_along] +
                                                split.second.extent[_along]);
    if (room == 0)
      return _ratio;
    const float first = at - static_cast<float>(_area->offset[_along]) -
                        static_cast<float>(seam_width) / 2.0f;
    const float ratio = std::clamp(first / static_cast<float>(room), 0.0f, 1.0f);
    return std::round(ratio * ratio_steps) / ratio_steps;
  }

  glm::length_t _along = 0; // 0 for x, 1 for y
  float _ratio = 0;
  std::string _name;
  std::string _view; // the one it runs in, where its ratio is set
  const VP_VIEW::Rect *_area = nullptr;
  VP_VIEW::Rect *_first = nullptr;
  VP_VIEW::Rect *_second = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  glm::vec2 _pointer{}; // as the events so far left it
  bool _dragging = false;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Split>("Split");
}
