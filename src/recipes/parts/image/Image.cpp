#include "contracts/Rect.h"
#include "contracts/Typed.h"
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
// area's edge, so it never draws over what is beside it; a press in its area gives it
// the focus, and while it has it, h brings it home, letterboxed again.
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
    // Only an image that moves takes keys, so one that stays still needs no connection
    // for them.
    if (_moves)
      _typed = &node.input<VP_VIEW::Typed>("typed");
    _name = node.name();
  }

  void cook(VP::Cook &frame) override {
    const bool room = _area->extent.x != 0 && _area->extent.y != 0;
    const std::span<VP_VIEW::Rect> place = frame.write(_place, room ? 1 : 0);
    if (room)
      place.front() = *_area;
    if (_moves) {
      keys(frame);
      move(frame);
    }
    frame.set(_pan, _view.pan);
    frame.set(_zoom, _view.zoom);
  }

  // Escape lets go of the focus, as the other parts that take keys do.
  void keys(VP::Cook &frame) {
    for (const VP::Event &event : _typed->to(_name)) {
      if (event.kind != VP::Event::Kind::key || !event.down)
        continue;
      if (event.name == PanZoom::home_key)
        _view.home();
      else if (event.name == "escape")
        frame.commands().send("focus");
    }
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void move(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events()) {
      const bool inside = VP_VIEW::contains(*_area, _pointer);
      if (event.kind == VP::Event::Kind::pointer) {
        if (_dragging)
          _view.pan += event.at - _pointer;
        _pointer = event.at;
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        _dragging = event.down && inside;
        if (_dragging && _typed->focus != _name)
          frame.commands().send(std::format("focus {}", _name));
      } else if (event.kind == VP::Event::Kind::wheel && inside) {
        _view.wheel(event.turn.y, _pointer - glm::vec2(_area->offset));
      }
    }
  }

  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr; // with moves = yes
  std::string _name;                      // as the focus names it
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
