#include "contracts/Rect.h"
#include "contracts/Still.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/PanZoom.h"
#include "shared/Params.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// "He shall be great, and shall be called the Son of the Highest: and the Lord God shall
// give unto him the throne of his father David:" Luke 1:32

constexpr std::string_view picture = "picture"; // the port its shaders sample
constexpr std::string_view still = "still"; // where a still says whether it holds a frame
// As near as the wheel zooms it: ten times the graph's, so a view's pixels show whole
// at a time; the graph keeps its own, as its boxes' text has nothing more to show.
constexpr float nearest = 10.0f * PanZoom::most;

// Seats the image in its area, as a panel seats any content, and its shaders letterbox
// it there. It draws one instance, or none while the area is empty, so an image given no
// room costs no draw. With moves = yes, a drag and the wheel move and zoom it, as in the
// graph; with interact = yes too, they go to the view whose window it shows instead. A
// still connected to it shows its held frame in place of the picture, moved and zoomed
// as the picture was, so the frame is looked at where it was.
class Image final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _area = &node.input<VP_VIEW::Rect>("area");
    _place = node.upload<VP_VIEW::Rect>("place");
    _pan = node.value<glm::vec2>("pan");
    _zoom = node.value<float>("zoom");
    const std::string moves = node.param<std::string>("moves");
    check_word("moves", moves, {"yes", "no"});
    _moves = moves == "yes";
    const std::string interact = node.param<std::string>("interact");
    check_word("interact", interact, {"yes", "no"});
    _interact = interact == "yes";
    // Only an image that moves takes keys, so one that stays still needs no connection
    // for them.
    if (_moves)
      _typed = &node.input<VP_VIEW::Typed>("typed");
    _name = node.name();
    // Only an image a still hands frames to reads whether one is held, so any other
    // needs no connection for it.
    _holds = node.value<std::uint32_t>("holds");
    const auto reaches = [&](const VP::Connection &connection) {
      return std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
        return to.node == _name && to.port == still;
      });
    };
    _still = std::ranges::any_of(node.view().connections, reaches)
                 ? &node.input<VP_VIEW::Still>(still)
                 : nullptr;
  }

  // With no room it shows nothing, so it says it reads no picture, and a view whose
  // window no reader shows rests.
  void cook(VP::Cook &frame) override {
    const bool room = _area->extent.x != 0 && _area->extent.y != 0;
    if (!room)
      frame.unread(picture);
    const std::span<VP_VIEW::Rect> place = frame.write(_place, room ? 1 : 0);
    if (room)
      place.front() = *_area;
    if (_moves) {
      follow(frame.view());
      keys(frame);
      if (_interact)
        hand(frame);
      else
        move(frame);
    }
    frame.set(_pan, _view.pan);
    frame.set(_zoom, _view.zoom);
    frame.set(_holds, static_cast<std::uint32_t>(_still && _still->holds));
  }

  // The writer of the connection that reaches its picture, as it was last, copied only
  // when another comes, so a frame allocates nothing for it. Another writer's picture, as
  // another view presented, starts at home. A picture that goes and comes back from the
  // same writer, as a pane's view shown again, stays where it was moved and zoomed.
  void follow(const VP::View &view) {
    const VP::Endpoint *from = nullptr;
    for (const VP::Connection &connection : view.connections)
      for (const VP::Endpoint &to : connection.to)
        if (to.node == _name && to.port == picture)
          from = &connection.from;
    if (!from || (from->node == _from.node && from->port == _from.port))
      return;
    _from = *from;
    _view.home();
  }

  // h brings the image home, letterboxed again, interacting or not. Escape lets go of
  // the focus, as the other parts that take keys do.
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

  // A press in its area gives it the focus. A left drag moves it, the wheel zooms it
  // about the pointer, and a middle drag zooms it about where it was pressed, nearer
  // going right, as in the graph. The events are read in order, so a press is tested
  // where the pointer was then.
  void move(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events()) {
      const bool inside = VP_VIEW::contains(*_area, _pointer);
      if (event.kind == VP::Event::Kind::pointer) {
        if (_dragging)
          _view.pan += event.at - _pointer;
        else if (_zooming)
          _view.wheel((event.at.x - _pointer.x) / PanZoom::drag_step,
                      *_zooming - glm::vec2(_area->offset),
                      PanZoom::least,
                      nearest);
        _pointer = event.at;
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        _dragging = event.down && inside;
        if (_dragging && _typed->focus != _name)
          frame.commands().send(std::format("focus {}", _name));
      } else if (event.kind == VP::Event::Kind::button && event.name == "middle") {
        _zooming =
            event.down && inside && !_dragging ? std::optional(_pointer) : std::nullopt;
      } else if (event.kind == VP::Event::Kind::wheel && inside) {
        _view.wheel(
            event.turn.y, _pointer - glm::vec2(_area->offset), PanZoom::least, nearest);
      }
    }
  }

  // A press in its area gives it the focus; once it has it, the pointer goes to the view
  // its picture is the window of, from the frame after, while it is over the area or a
  // press made there is not let go, which the image sees as the view does. The picture
  // is that view's whole window, as large as the frame, so the view reads the pointer
  // over its window where the picture shows under it: at the picture's edge, or past it
  // beside the picture, at its window's edge, as a pointer at a screen's edge.
  void hand(VP::Cook &frame) {
    const bool focused = _typed->focus == _name;
    for (const VP::Event &event : frame.input().events()) {
      const bool inside = VP_VIEW::contains(*_area, _pointer);
      if (event.kind == VP::Event::Kind::pointer) {
        _pointer = event.at;
      } else if (event.kind == VP::Event::Kind::button) {
        if (event.down && inside && !focused)
          frame.commands().send(std::format("focus {}", _name));
        _dragging = event.down ? _dragging || (inside && focused) : false;
      }
    }
    if (!_from.view() || !focused || (!_dragging && !VP_VIEW::contains(*_area, _pointer)))
      return;
    const auto [at, size] =
        _view.shows(glm::vec2(_area->extent), glm::vec2(frame.resolution()));
    frame.hand_pointer(_from.node, glm::vec2(_area->offset) + at, size);
  }

  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr; // with moves = yes
  const VP_VIEW::Still *_still = nullptr; // while a still is connected to it
  std::string _name;                      // as the focus names it
  VP::Upload<VP_VIEW::Rect> _place;
  VP::Value<glm::vec2> _pan;
  VP::Value<float> _zoom;
  VP::Value<std::uint32_t> _holds;
  PanZoom _view;
  VP::Endpoint _from;     // what reached its picture last
  glm::vec2 _pointer{};   // as the events so far left it
  bool _moves = false;    // whether a drag and the wheel move it
  bool _interact = false; // whether they go to the view it shows instead
  bool _dragging = false; // from a press in its area until it is let go, handed or not
  // Where a middle press in its area was, which a drag sideways zooms about until it is
  // let go.
  std::optional<glm::vec2> _zooming;
};

} // namespace

VP_OPERATOR(Image);
