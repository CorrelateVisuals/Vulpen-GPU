#include "contracts/Rect.h"
#include "contracts/Still.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view picture = "picture"; // the port its shader copies from
constexpr std::string_view held_port = "held";   // and stores the frame into
constexpr std::string_view hold_key = "space";

// Holds a frame of its picture still, for a person to look at while what draws it goes
// on: space, while the node its host param names has the focus, or else the node its held
// reaches, as the image showing the frame, or a double click in its area, copies the
// picture into held at once, its shader storing it the frame it was asked, and still
// tells the image held reaches to show it in place of the picture; either again lets it
// go. The frame is of what wrote the picture then, as a project's window, and it holds
// while that writes it: through the picture's connection going and coming back, as a
// pane's view shown again, and while its area has no room, as in perform mode or under a
// preview; a picture another writes shows as it is, and holding it lets go of the first,
// as does a change of the window's size, which makes held anew, empty. A key held down
// holds once, however often it repeats. It changes nothing a view holds, so it sends no
// line: a frame held is a look at the view, as a pan of the image is.
class Still final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _name = node.name();
    _host = host_of(node);
    _area = &node.input<VP_VIEW::Rect>("area");
    _typed = &node.input<VP_VIEW::Typed>("typed");
    _still = &node.output<VP_VIEW::Still>("still");
    _shoot = node.value<std::uint32_t>("shoot");
  }

  // With no writer reaching its picture, as a pane no deck shows, keys and presses ask
  // nothing, so they never let go of a frame held before.
  void cook(VP::Cook &frame) override {
    follow(frame.view());
    const bool asked = keys() != presses(frame) && !_from.node.empty();
    if (frame.resolution() != _size)
      _held_of.reset();
    _size = frame.resolution();
    const bool holds = _held_of && same(*_held_of, _from);
    if (asked)
      _held_of = holds ? std::nullopt : std::optional(_from);
    _still->holds = _held_of && same(*_held_of, _from);
    frame.set(_shoot, static_cast<std::uint32_t>(asked && !holds));
  }

  std::string host_of(VP::Bind &node) const {
    if (node.sets("host"))
      return node.param<std::string>("host");
    for (const VP::Connection &connection : node.view().connections)
      if (connection.from.node == _name && connection.from.port == held_port &&
          !connection.to.empty())
        return connection.to.front().node;
    throw std::runtime_error(
        std::format("{} names no host param, nor does its held reach a node", _name));
  }

  static bool same(const VP::Endpoint &one, const VP::Endpoint &other) {
    return one.node == other.node && one.port == other.port;
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

  // The writer of the connection that reaches its picture; copied only as it changes, so
  // a frame allocates nothing for it.
  void follow(const VP::View &view) {
    const VP::Endpoint *from = nullptr;
    for (const VP::Connection &connection : view.connections)
      for (const VP::Endpoint &to : connection.to)
        if (to.node == _name && to.port == picture)
          from = &connection.from;
    if (from ? !same(*from, _from) : !_from.node.empty())
      _from = from ? *from : VP::Endpoint{};
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
  // What wrote the frame held; none while none is.
  std::optional<VP::Endpoint> _held_of;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Still>("Still");
}
