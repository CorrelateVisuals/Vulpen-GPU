#include "contracts/Rect.h"
#include "contracts/Still.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view picture = "picture"; // the port its shader copies from
constexpr std::string_view hold_key = "space";

// Holds a frame of its picture still, for a person to look at while what draws it goes
// on: space, while the node its host param names has the focus, or a double click in its
// area, copies the picture into held at once, its shader storing it the frame it was
// asked, and still tells the image held reaches to show it in place of the picture;
// either again lets it go. It lets go too once it shows nowhere, as its area has no room
// in perform mode, once a connection brings its picture anew, as another view presented,
// and once the window's size changes, which makes held anew, empty. A key held down holds
// once, however often it repeats. It changes nothing a view holds, so it sends no line:
// a frame held is a look at the view, as a pan of the image is.
class Still final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _host = node.param<std::string>("host");
    _name = node.name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _still = &node.output<VP_VIEW::Still>("still");
    _shoot = node.value<std::uint32_t>("shoot");
  }

  void cook(VP::Cook &frame) override {
    const bool asked = keys() != presses(frame);
    if (follow(frame.view()) || frame.resolution() != _size ||
        _area->extent.x == 0 || _area->extent.y == 0)
      _still->holds = false;
    _size = frame.resolution();
    const bool shoots = asked && !_still->holds;
    _still->holds = _still->holds != asked;
    frame.set(_shoot, static_cast<std::uint32_t>(shoots));
  }

  // Whether the host's keys ask an odd number of times: space pressed, not repeated.
  bool keys() {
    if (_typed->focus != _host)
      _held = false;
    bool asked = false;
    for (const VP::Event &event : _typed->to(_host))
      if (event.kind == VP::Event::Kind::key && event.name == hold_key) {
        asked = asked != (event.down && !_held);
        _held = event.down;
      }
    return asked;
  }

  // Whether double clicks in its area ask an odd number of times, each tested where the
  // pointer was as it came.
  bool presses(VP::Cook &frame) {
    bool asked = false;
    for (const VP::Event &event : frame.input().events())
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::button && event.name == "left" &&
               event.down && event.presses == 2 && VP_VIEW::contains(*_area, _pointer))
        asked = !asked;
    return asked;
  }

  // Whether the writer of the connection that reaches its picture changed since the frame
  // before; copied only then, so a frame allocates nothing for it.
  bool follow(const VP::View &view) {
    const VP::Endpoint *from = nullptr;
    for (const VP::Connection &connection : view.connections)
      for (const VP::Endpoint &to : connection.to)
        if (to.node == _name && to.port == picture)
          from = &connection.from;
    if (from ? from->node == _from.node && from->port == _from.port : _from.node.empty())
      return false;
    _from = from ? *from : VP::Endpoint{};
    return true;
  }

  std::string _host; // the node whose keys it reads, as the focus names it
  std::string _name;
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  VP_VIEW::Still *_still = nullptr;
  VP::Value<std::uint32_t> _shoot;
  VP::Endpoint _from;   // what reaches its picture, as the frame before saw it
  glm::uvec2 _size{};   // the window's, as the frame before had it
  glm::vec2 _pointer{}; // as the events so far left it
  bool _held = false;   // space down, as the host's keys left it
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Still>("Still");
}
