#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Presented.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view tabs_port = "tabs";
constexpr std::string_view typed_port = "typed"; // the keys part's, for the focus
// Its tab while it holds no view hosted, so its strip has a tab to take, as an empty
// editor's has.
constexpr std::string_view empty_tab = "perform";

// Holds views by name, as its holds param names them, and shows one in the Rect its area
// gives: the view presented while it holds that one, else the one its shown param names,
// else the first it holds; each port of it but tabs, named for a view, reaches the pane
// showing that view, as the panes part connects it, and the one shown gets the room, the
// others none. A tab a view it holds that is hosted, in the order its holds param names
// them, whose press presents the view, and so shows it here, the one presented labeled in
// the string role's green, as perform mode shows that one, its close mark closing the
// view; with none, a tab of its own, named perform, which does nothing pressed. Its shown
// param follows the view presented as that comes to one it holds, so a save keeps the one
// it shows and the docking part finds it. The focus on the pane it hides, while its host
// connects the keys part's typed to its typed, goes to the pane it shows, the node of it
// listed where the other's was, so keys go to the view seen. Showing none, it takes the
// focus itself at a press in its room, as an empty editor does, so the panel holding it
// is framed as the one in focus, and escape lets go of it; the first view it shows then
// takes it, at the pane's first node.
class Deck final : public VP::Operator {
  // A port to a pane: the view it is named for, the Rect it hands, and the nodes it
  // reaches, in the order the connection lists them.
  struct Room {
    std::string view;
    VP_VIEW::Rect *rect = nullptr;
    std::vector<std::string> nodes;
  };

  void bind(VP::Bind &node) override {
    _name = node.name();
    _view = node.view_name();
    _holds = node.sets(holds_param) ? split_names(node.param<std::string>(holds_param))
                                    : std::vector<std::string>();
    _shown_param =
        node.sets(shown_param) ? node.param<std::string>(shown_param) : std::string();
    _area = &node.input<VP_VIEW::Rect>("area");
    _tabs = &node.output<VP_VIEW::Items>(tabs_port);
    _typed = nullptr;
    _rooms.clear();
    for (const VP::Connection &connection : node.view().connections) {
      if (std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == typed_port;
          }))
        _typed = &node.input<VP_VIEW::Typed>(typed_port);
      if (connection.from.node != _name || connection.from.port == tabs_port)
        continue;
      Room room{.view = connection.from.port,
                .rect = &node.output<VP_VIEW::Rect>(connection.from.port)};
      for (const VP::Endpoint &to : connection.to)
        room.nodes.push_back(to.node);
      _rooms.push_back(std::move(room));
    }
    _named = false; // its tabs anew, for what it holds now
  }

  // The views are read before a line goes, as one replaces them.
  void cook(VP::Cook &frame) override {
    const VP::View &top = *frame.view("");
    auto hosted = projects(top, _view);
    if (!std::ranges::equal(_hosted, hosted, {}, {}, &VP::Child::name)) {
      _hosted.clear();
      for (const VP::Child &child : hosted)
        _hosted.push_back(child.name);
      _named = false;
    }
    const std::string_view presented = shown_by(frame.view());
    if (presented != _presented) {
      _presented = presented;
      _named = false;
    }
    if (!std::exchange(_named, true))
      choose(frame.commands(), frame.view());
    for (const Room &room : _rooms)
      *room.rect = room.view == _shows ? *_area : VP_VIEW::Rect{};
    if (_typed)
      take_focus(frame);
  }

  // In the order the events came, so a press is tested where the pointer was then.
  void take_focus(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events())
      if (event.kind == VP::Event::Kind::pointer)
        _pointer = event.at;
      else if (event.kind == VP::Event::Kind::button && event.down &&
               event.name == "left" && _shows.empty() &&
               VP_VIEW::contains(*_area, _pointer) && _typed->focus != _name)
        frame.commands().send(std::format("{}: focus {}", _view, _name));
    for (const VP::Event &event : _typed->to(_name))
      if (event.kind == VP::Event::Kind::key && event.down && event.name == "escape")
        frame.commands().send(std::format("{}: focus", _view));
  }

  // The view it shows, its tabs, and the focus and its shown param following them; only
  // as what it holds, the views hosted or the one presented change, so a frame allocates
  // nothing for them.
  void choose(VP::CommandPort &commands, const VP::View &own) {
    const auto self = std::ranges::find(own.nodes, _name, &VP::Node::name);
    const auto hosts = [&](std::string_view name) {
      return std::ranges::find(_hosted, name) != _hosted.end();
    };
    std::string shows =
        self == own.nodes.end() ? std::string() : deck_shows(*self, _presented, hosts);
    if (shows != _shows)
      refocus(commands, std::exchange(_shows, std::move(shows)));
    if (!_shows.empty() && _shows == _presented && _shows != _shown_param)
      commands.send(
          std::format("{}: param set {} {} {}", _view, _name, shown_param, _shows));
    _tabs->clear();
    for (const std::string &view : _holds)
      if (hosts(view))
        _tabs->push_back(
            {.label = view,
             .command = std::format("view switch {}", view),
             .current = view == _shows,
             .close = std::format("view close {}", view),
             .role = view == _presented ? std::optional(VP_VIEW::role("string"))
                                        : std::nullopt});
    if (_tabs->empty())
      _tabs->push_back({.label = std::string(empty_tab), .current = true});
  }

  // The focus on the pane it hid goes to the node of the pane it shows listed where the
  // focused one was in its own, and the focus on itself to the pane's first node.
  void refocus(VP::CommandPort &commands, std::string_view hid) const {
    const auto room_of = [&](std::string_view view) {
      return std::ranges::find(_rooms, view, &Room::view);
    };
    const auto was = room_of(hid);
    const auto now = room_of(_shows);
    if (!_typed || now == _rooms.end() || now->nodes.empty())
      return;
    if (_typed->focus == _name) {
      commands.send(std::format("{}: focus {}", _view, now->nodes.front()));
      return;
    }
    if (was == _rooms.end())
      return;
    const auto focus = std::ranges::find(was->nodes, _typed->focus);
    const auto listed = static_cast<std::size_t>(focus - was->nodes.begin());
    if (focus != was->nodes.end() && listed < now->nodes.size())
      commands.send(std::format("{}: focus {}", _view, now->nodes[listed]));
  }

  std::string _name;
  std::string _view; // the one it runs in, which its lines change
  std::vector<std::string> _holds; // as its param names them
  std::string _shown_param;
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr; // while its host connects it
  VP_VIEW::Items *_tabs = nullptr;
  std::vector<Room> _rooms;
  std::vector<std::string> _hosted; // the views hosted, as the frame before found them
  std::string _presented;
  std::string _shows; // the view it shows; empty for none
  bool _named = false; // its tabs and the view it shows follow what is
  glm::vec2 _pointer{}; // as the events so far left it
};

} // namespace

VP_OPERATOR(Deck);
