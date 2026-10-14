#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "shared/PanZoom.h"

#include <format>
#include <span>
#include <stdexcept>
#include <string>

namespace {

// Seats the image in the Rect its area gives, as a panel seats any content, and its
// shaders letterbox it there: one instance of its draw, none while the area is empty, so
// an image given no room costs no draw. With param moves = yes, a drag moves it and the
// wheel zooms it about the pointer, as the graph moves, and its shaders cut it at the
// area's edge, so it never draws over what is beside it.
class Image final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _area = &node.input<VP_VIEW::Rect>("area");
    _place = node.upload<VP_VIEW::Rect>("place");
    _pan = node.value<glm::vec2>("pan");
    _zoom = node.value<float>("zoom");
    const std::string moves = node.param<std::string>("moves");
    if (moves != "yes" && moves != "no")
      throw std::runtime_error(std::format("param moves = {}: yes or no", moves));
    _moves = moves == "yes";
  }

  void cook(VP::Cook &frame) override {
    const bool room = _area->extent.x != 0 && _area->extent.y != 0;
    const std::span<VP_VIEW::Rect> place = frame.write(_place, room ? 1 : 0);
    if (room)
      place.front() = *_area;
    if (_moves)
      move(frame.input());
    frame.set(_pan, _view.pan);
    frame.set(_zoom, _view.zoom);
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void move(const VP::InputPort &input) {
    for (const VP::Event &event : input.events()) {
      const bool inside = VP_VIEW::contains(*_area, _pointer);
      if (event.kind == VP::Event::Kind::pointer) {
        if (_dragging)
          _view.pan += event.at - _pointer;
        _pointer = event.at;
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        _dragging = event.down && inside;
      } else if (event.kind == VP::Event::Kind::wheel && inside) {
        _view.wheel(event.turn.y, _pointer - glm::vec2(_area->offset));
      }
    }
  }

  const VP_VIEW::Rect *_area = nullptr;
  VP::Upload<VP_VIEW::Rect> _place;
  VP::Value<glm::vec2> _pan;
  VP::Value<float> _zoom;
  PanZoom _view;
  glm::vec2 _pointer{};   // as the events so far left it
  bool _moves = false;    // whether a drag and the wheel move it
  bool _dragging = false; // from a press in its area until it is let go
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Image>("Image");
}
