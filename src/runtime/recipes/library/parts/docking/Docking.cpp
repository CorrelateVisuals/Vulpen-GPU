#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Halves.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The inner nodes of the dock and panel components that take the Rect they lay out at
// their area: a dock's split, which divides it, and a panel's bar, which cuts the panel's
// tab strip off its top.
constexpr std::string_view split_node = ".split";
constexpr std::string_view bar_node = ".bar";
constexpr std::string_view area_port = "area";
// What a panel's button shows: its name, but this, as the ide names its panels.
constexpr std::string_view panel_suffix = "-panel";
// The Rects a dock's split hands its two halves, in the order the ratio counts them.
constexpr std::array<std::string_view, 2> half_ports{"first", "second"};
constexpr std::array<std::string_view, 4> sides{"left", "right", "top", "bottom"};
// Into a panel, which then holds both under one strip of their tabs.
constexpr std::string_view middle = "middle";
// How far in from a panel's edges, as a share of it, a drop goes into it.
constexpr float middle_from = 0.25f;
// The ports a panel takes its content's room and tabs at, and the nodes of a stack: its
// switch, which hands one content the room, and its tabs, which joins theirs.
constexpr std::string_view rest_port = "rest";
constexpr std::string_view tabs_node = ".tabs";
constexpr std::string_view hit_node = ".hit";
constexpr std::string_view items_port = "items";
constexpr std::string_view tabs_port = "tabs";
constexpr std::string_view switch_node = ".switch";
constexpr std::string_view switch_tabs_node = ".tabs";
// The recipes a move adds: a stack for a panel holding two contents, a panel for one torn
// out of a stack, and a dock to seat it.
constexpr std::string_view stack_recipe = "stack";
constexpr std::string_view panel_recipe = "panel";
constexpr std::string_view dock_recipe = "dock";
constexpr float even = 0.5f;      // a moved dock's ratio: half its place for each panel
constexpr float drag_from = 8.0f; // pixels the pointer goes from a press before it drags
constexpr std::int32_t ring = 2;  // how wide the frame where a drop puts a panel is
constexpr std::uint32_t frame_rects = 4;

// A panel a dock seats, where it shows, and its tab strip, which a drag takes it by, and
// where the view lists its bar, which orders the buttons.
struct Seated {
  std::string panel;
  VP_VIEW::Rect area;
  VP_VIEW::Rect strip;
  std::size_t listed = 0;
};

// The dock's split seating a node, by the node its area endpoint names, as ide.top.split
// or ide.editor-panel.bar: which half, and the dock's ratio, which a dock that gives none
// takes as halving.
struct Seating {
  std::string split;
  std::size_t half = 0;
  float ratio = 0.0f;

  // Whether the ratio gives a half no room: 0 the first, 1 the second.
  bool gives_none(std::size_t of) const {
    return of == 0 ? ratio <= 0.0f : ratio >= 1.0f;
  }
};

template <class T> T number(std::string_view text) {
  T value{};
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

std::string_view param_of(const VP::Node &node, std::string_view key) {
  const auto found = std::ranges::find(node.params, key, &VP::Param::key);
  return found == node.params.end() ? std::string_view() : std::string_view(found->value);
}

// What a panel holds: the ports its room goes to, and the port its tabs come from, as a
// stack's half holds a content too.
struct Content {
  std::vector<VP::Endpoint> area;
  VP::Endpoint tabs;
};

// The connection that hands a node's port its Rect; null for none.
const VP::Connection *feeding(const VP::View &view, std::string_view node) {
  for (const VP::Connection &connection : view.connections)
    for (const VP::Endpoint &to : connection.to)
      if (to.node == node && to.port == area_port)
        return &connection;
  return nullptr;
}

const VP::Connection *named(const VP::View &view, std::string_view name) {
  const auto found = std::ranges::find(view.connections, name, &VP::Connection::name);
  return found == view.connections.end() ? nullptr : &*found;
}

// As the view the lines so far left holds it, so a line before the first frame finds it
// too; none for a node no dock's split seats.
std::optional<Seating> seating_of(const VP::View &view, std::string_view seated) {
  const VP::Connection *const into = feeding(view, seated);
  if (!into || !into->from.node.ends_with(split_node))
    return std::nullopt;
  Seating seating{.split = into->from.node,
                  .half = into->from.port == half_ports[0] ? 0u : 1u,
                  .ratio = 0.5f};
  const std::string_view dock =
      std::string_view(seating.split).substr(0, seating.split.size() - split_node.size());
  const auto node = std::ranges::find(view.nodes, dock, &VP::Node::name);
  if (node != view.nodes.end())
    if (const std::string_view ratio = param_of(*node, "split.ratio"); !ratio.empty())
      seating.ratio = number<float>(ratio);
  return seating;
}

// The connection from a port; null for none.
const VP::Connection *
from(const VP::View &view, std::string_view node, std::string_view port) {
  const auto found = std::ranges::find_if(view.connections, [&](const VP::Connection &c) {
    return c.from.node == node && c.from.port == port;
  });
  return found == view.connections.end() ? nullptr : &*found;
}

// The connection into a port; null for none.
const VP::Connection *
into(const VP::View &view, std::string_view node, std::string_view port) {
  const auto found = std::ranges::find_if(view.connections, [&](const VP::Connection &c) {
    return std::ranges::any_of(c.to, [&](const VP::Endpoint &to) {
      return to.node == node && to.port == port;
    });
  });
  return found == view.connections.end() ? nullptr : &*found;
}

// What a panel holds: what its bar's rest feeds and what feeds its tabs.
std::optional<Content> content_of(const VP::View &view, std::string_view panel) {
  const std::string bar = std::string(panel) + std::string(bar_node);
  const VP::Connection *const room = from(view, bar, rest_port);
  const VP::Connection *const tabs =
      into(view, std::string(panel) + std::string(tabs_node), items_port);
  if (!room || !tabs)
    return std::nullopt;
  return Content{.area = room->to, .tabs = tabs->from};
}

// What a stack's half holds: what its switch's half feeds, but the stack's own tabs, and
// what feeds its tabs.
std::optional<Content>
half_of(const VP::View &view, std::string_view stack, std::size_t half) {
  const std::string tabs_of = std::string(stack) + std::string(switch_tabs_node);
  const VP::Connection *const room =
      from(view, std::string(stack) + std::string(switch_node), half_ports[half]);
  const VP::Connection *const tabs =
      into(view, tabs_of, std::format("{}-{}", half_ports[half], tabs_port));
  if (!room || !tabs)
    return std::nullopt;
  Content content{.area = room->to, .tabs = tabs->from};
  std::erase_if(content.area,
                [&](const VP::Endpoint &to) { return to.node == tabs_of; });
  return content;
}

// The stack a panel's room goes to, by its switch; empty for none.
std::string stack_of(const VP::View &view, const Content &held) {
  const std::string_view node = held.area.front().node;
  if (!node.ends_with(switch_node))
    return {};
  const std::string stack(node.substr(0, node.size() - switch_node.size()));
  const auto found = std::ranges::find(view.nodes, stack, &VP::Node::name);
  return found != view.nodes.end() && found->recipe == stack_recipe ? stack
                                                                     : std::string();
}

// The node a content is, past the node the view's nodes are inside: terminal for
// ide.terminal.command-line, perform for ide.perform.
std::string_view content_name(const Content &content) {
  const std::string_view node = content.area.front().node;
  const std::size_t dot = node.find('.');
  if (dot == std::string_view::npos)
    return node;
  const std::string_view inside = node.substr(dot + 1);
  return inside.substr(0, inside.find('.'));
}

// The node a node is inside, as ide of ide.editor-panel; empty at the top.
std::string_view parent_of(std::string_view node) {
  return node.substr(0, std::min(node.rfind('.'), node.size()));
}

// The node name wanted, or with a number after it while a node has it.
std::string free_node(const VP::View &view, std::string_view wanted) {
  std::string name(wanted);
  for (std::size_t count = 2;
       std::ranges::find(view.nodes, name, &VP::Node::name) != view.nodes.end();
       ++count)
    name = std::format("{}{}", wanted, count);
  return name;
}

// A connection named for what it seats, as the manifest's own are, past the last dot.
std::string_view short_name(std::string_view node) {
  return node.substr(node.rfind('.') + 1);
}

// The half of a panel's place a drop at a side gives the panel dropped, or all of it, as
// a drop into its middle shares it.
VP_VIEW::Rect half_at(const VP_VIEW::Rect &area, std::string_view side) {
  if (side == middle)
    return area;
  const glm::length_t along = side == "left" || side == "right" ? 0 : 1;
  const Halves split = halves(area, along, even);
  return side == "left" || side == "top" ? split.first : split.second;
}

// Moves a panel of a dock beside another, at its left, right, top or bottom, as edits of
// the docks that seat them in its own view's manifest: the dock seating the panel moves
// with it, its other half taking its place, into the place of the panel dropped on, and
// holds the two, halved; one log group, which a save keeps and a replay rebuilds (V08).
// Into another's middle, the panel's content joins the other's in a stack, which holds
// both under one strip, and the panel goes; from a panel holding several, the content it
// shows leaves, into a panel of its own when dropped beside one, which it adds before
// itself, as it lists before what draws over the panels. A drag of a panel's tab strip
// onto another panel sends it, the side the edge of that panel the pointer is nearest,
// or the middle well inside it. It hides a panel and shows it again, as its dock's ratio
// gives its half no room and back, and hands on a button a panel, lit while it shows,
// whose press hides or shows it, as the POC's menubar cells did. It finds where each
// panel and its strip show as the splits and the bars lay them out, from the Rect its
// area gives, which its host also hands the outermost dock, and while a drag goes on it
// frames where a drop would put the panel dragged.
class Docking final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _move = node.command("panel move <node> <node> <value>",
                         "moves a panel beside another, at its left, right, top or "
                         "bottom, or into it, middle, which then holds both; of a panel "
                         "holding several, the one it shows");
    _hide = node.command("panel hide <node>",
                         "gives a panel no room, its dock's ratio leaving its half none, "
                         "or the dock's own when nothing beside it shows");
    _show = node.command("panel show <node>",
                         "gives a panel hidden its room again, as its docks' ratios were "
                         "before it hid, or halved");
    _buttons = &node.output<VP_VIEW::Items>("buttons");
    _name = node.name();
    _view = node.view_name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _font = &node.input<VP_VIEW::Font>("font");
    _rects = node.upload<VP_VIEW::Rect>("rects", frame_rects);
    _laid_out.reset(); // a rebuild may have moved every panel
  }

  void cook(VP::Cook &frame) override {
    const glm::uvec2 cell = _font->cell;
    if (const Laid laid{_area->offset, _area->extent, cell}; _laid_out != laid) {
      _laid_out = laid;
      seat_all(frame.view(), cell);
    }
    for (const VP::Event &event : frame.input().events())
      follow(frame, event);
    const auto drop = dropped();
    frame.write(_rects,
                drop ? ring_round(frame.write(_rects),
                                  half_at(_seated[drop->first].area, drop->second))
                     : 0);
  }

  // From the dock its area's connection hands the same Rect, as far down as docks go;
  // then the buttons, in the order the view lists the panels, as they changed.
  void seat_all(const VP::View &view, glm::uvec2 cell) {
    _seated.clear();
    if (const VP::Connection *const root = feeding(view, _name))
      for (const VP::Endpoint &to : root->to)
        if (to.node != _name)
          seat(view, to, *_area, cell);
    std::ranges::stable_sort(_seated, {}, &Seated::listed);
    write_buttons();
  }

  // A button a panel, lit while it shows, only as they change, so a frame allocates
  // nothing for them (CPP10).
  void write_buttons() {
    VP_VIEW::Items buttons;
    for (const Seated &seated : _seated) {
      std::string_view label = short_name(seated.panel);
      if (label.ends_with(panel_suffix))
        label.remove_suffix(panel_suffix.size());
      const bool shows = seated.area.extent.x != 0 && seated.area.extent.y != 0;
      buttons.push_back(
          {.label = std::string(label),
           .command = std::format("panel {} {}", shows ? "hide" : "show", seated.panel),
           .current = shows});
    }
    if (buttons != *_buttons)
      *_buttons = std::move(buttons);
  }

  void seat(const VP::View &view,
            const VP::Endpoint &end,
            const VP_VIEW::Rect &area,
            glm::uvec2 cell) {
    const auto node = std::ranges::find(view.nodes, end.node, &VP::Node::name);
    if (node == view.nodes.end() || end.port != area_port)
      return;
    if (end.node.ends_with(split_node)) {
      const glm::length_t along = param_of(*node, "axis") == "y" ? 1 : 0;
      const float ratio = number<float>(param_of(*node, "ratio"));
      const Halves split = halves(area, along, ratio);
      for (const VP::Connection &connection : view.connections)
        if (connection.from.node == end.node && connection.from.port != "rects")
          for (const VP::Endpoint &to : connection.to)
            seat(view,
                 to,
                 connection.from.port == half_ports[0] ? split.first : split.second,
                 cell);
    } else if (end.node.ends_with(bar_node)) {
      VP_VIEW::Rect strip = area;
      strip.extent.y = bar_length(
          number<std::uint32_t>(param_of(*node, "size")), cell.y, area.extent.y);
      const std::string_view panel =
          std::string_view(end.node).substr(0, end.node.size() - bar_node.size());
      _seated.push_back({.panel = std::string(panel),
                         .area = area,
                         .strip = strip,
                         .listed = static_cast<std::size_t>(node - view.nodes.begin())});
    }
  }

  // A press on a strip grabs its panel, a drag past a few pixels takes it, and letting go
  // on another panel moves it there.
  void follow(VP::Cook &frame, const VP::Event &event) {
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      _dragging = _dragging || (_grabbed && glm::distance(_pointer, _from) > drag_from);
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      if (event.down) {
        const auto pressed = std::ranges::find_if(_seated, [&](const Seated &seated) {
          return VP_VIEW::contains(seated.strip, _pointer);
        });
        _grabbed = pressed == _seated.end()
                       ? std::nullopt
                       : std::optional(std::size_t(pressed - _seated.begin()));
        _from = _pointer;
      } else {
        if (const auto drop = dropped())
          frame.commands().send(std::format("{}: panel move {} {} {}",
                                            _view,
                                            _seated[*_grabbed].panel,
                                            _seated[drop->first].panel,
                                            drop->second));
        _grabbed.reset();
        _dragging = false;
      }
    }
  }

  // The panel a drag would drop the one taken on, by the edge the pointer is nearest.
  std::optional<std::pair<std::size_t, std::string_view>> dropped() const {
    if (!_dragging || !_grabbed)
      return std::nullopt;
    for (std::size_t at = 0; at < _seated.size(); ++at) {
      const VP_VIEW::Rect &area = _seated[at].area;
      if (at == *_grabbed || !VP_VIEW::contains(area, _pointer))
        continue;
      const glm::vec2 in = (_pointer - glm::vec2(area.offset)) / glm::vec2(area.extent);
      const std::array<float, 4> near{in.x, 1.0f - in.x, in.y, 1.0f - in.y};
      const auto nearest = std::ranges::min_element(near);
      return std::pair(at,
                       *nearest > middle_from ? middle : sides[nearest - near.begin()]);
    }
    return std::nullopt;
  }

  // A frame round a Rect, ring pixels wide, inside it.
  static std::uint32_t ring_round(std::span<VP_VIEW::Rect> rects,
                                  const VP_VIEW::Rect &at) {
    const glm::ivec2 low = at.offset;
    const glm::uvec2 size = at.extent;
    const auto wide = static_cast<std::uint32_t>(ring);
    const std::uint32_t role = VP_VIEW::role("accent");
    rects[0] = {.offset = low, .extent = {size.x, wide}, .role = role};
    rects[1] = {.offset = low + glm::ivec2(0, static_cast<std::int32_t>(size.y) - ring),
                .extent = {size.x, wide},
                .role = role};
    rects[2] = {.offset = low, .extent = {wide, size.y}, .role = role};
    rects[3] = {.offset = low + glm::ivec2(static_cast<std::int32_t>(size.x) - ring, 0),
                .extent = {wide, size.y},
                .role = role};
    return frame_rects;
  }

  void command(VP::Call &call) override {
    if (call.is(_move))
      move(call);
    else if (call.is(_hide))
      hide(call, std::string(call.arguments().front()));
    else if (call.is(_show))
      show(call, std::string(call.arguments().front()));
  }

  // The dock seating the panel gives its half no room, or, when its other half shows
  // nothing either, the dock's own dock gives the dock none, and so up, so nothing
  // hidden keeps a seam.
  void hide(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    std::optional<Seating> seating = seating_of(view, panel + std::string(bar_node));
    if (!seating)
      throw std::runtime_error(std::format("{} is no panel a dock seats", panel));
    for (; seating; seating = seating_of(view, seating->split)) {
      if (seating->gives_none(seating->half))
        return;
      if (!seating->gives_none(1 - seating->half)) {
        _ratios[seating->split] = seating->ratio;
        send(call,
             std::format("param set {} ratio {}", seating->split, seating->half));
        return;
      }
    }
    throw std::runtime_error(std::format("{} is the last panel shown", panel));
  }

  // Every dock above the panel gives its half room again: at the ratio it had before it
  // gave none, or halved; but a dock that a dock above it hid gives the panel's half all
  // of it, as what else it holds was hidden before.
  void show(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    std::vector<Seating> way; // from the panel up, read before a line changes the view
    for (std::optional<Seating> seating = seating_of(view, panel + std::string(bar_node));
         seating;
         seating = seating_of(view, seating->split))
      way.push_back(*seating);
    if (way.empty())
      throw std::runtime_error(std::format("{} is no panel a dock seats", panel));
    for (auto seating = way.begin(); seating != way.end(); ++seating) {
      if (!seating->gives_none(seating->half))
        continue;
      const bool above = std::any_of(seating + 1, way.end(), [](const Seating &up) {
        return up.gives_none(up.half);
      });
      const auto kept = _ratios.find(seating->split);
      const float ratio = above ? (seating->half == 0 ? 1.0f : 0.0f)
                          : kept != _ratios.end() && kept->second > 0.0f &&
                                  kept->second < 1.0f
                              ? kept->second
                              : even;
      send(call, std::format("param set {} ratio {}", seating->split, ratio));
    }
  }

  // The panels are its own view's, whichever view the line addressed, and every edit
  // names that view, in one group. A panel holding one content moves whole: beside the
  // target, or into it, which then holds both; a panel holding several, as a stack, lets
  // go of the one it shows, which goes into the target, or into a panel of its own
  // beside it.
  void move(VP::Call &call) {
    const std::string panel(call.arguments()[0]);
    const std::string target(call.arguments()[1]);
    const std::string side(call.arguments()[2]);
    if (side != middle && std::ranges::find(sides, side) == sides.end())
      throw std::runtime_error(std::format(
          "side {}: left, right, top, bottom or middle of {}", side, target));
    const VP::View &view = *call.view(_view);
    const VP::Connection *const into = feeding(view, panel + std::string(bar_node));
    const std::optional<Content> held = content_of(view, panel);
    if (panel == target || !into || !into->from.node.ends_with(split_node) || !held ||
        !feeding(view, target + std::string(bar_node)) || !content_of(view, target))
      throw std::runtime_error(std::format(
          "{} moves beside or into another panel a dock seats, not {}; ls lists the "
          "nodes",
          panel,
          target));
    if (!stack_of(view, *held).empty()) {
      const Content going = unstack(call, panel);
      if (side == middle)
        join(call, going, target);
      else
        seat_new(call, going, panel, target, side);
    } else if (side == middle) {
      const Content going = *held;
      leave(call, panel);
      send(call, std::format("node remove {}", panel));
      join(call, going, target);
    } else {
      move_beside(call, panel, target, side);
    }
  }

  // The dock seating the panel moves with it: its other half takes its place, and it
  // takes the target's place, its halves the two, so every dock stays listed before the
  // panels whose ground it draws first, and a connection is named for what it seats, as
  // the manifest's own are.
  void move_beside(VP::Call &call,
                   const std::string &panel,
                   const std::string &target,
                   std::string_view side) {
    const VP::View &view = *call.view(_view);
    const VP::Connection *const into = feeding(view, panel + std::string(bar_node));
    const VP::Connection *const beside = feeding(view, target + std::string(bar_node));
    const std::string split = into->from.node;
    const std::string_view other =
        into->from.port == half_ports[0] ? half_ports[1] : half_ports[0];
    const VP::Connection *const rest = from(view, split, other);
    const VP::Connection *const above = feeding(view, split);
    if (!rest || !above)
      throw std::runtime_error(
          std::format("{} seats {} alone, or has no Rect", split, panel));
    // Copied first: each edit replaces the view these point into.
    const VP::Connection into_panel = *into, into_target = *beside, into_rest = *rest,
                         into_split = *above;
    const bool sibling = into_target.name == into_rest.name;
    send(call, std::format("disconnect {}", into_panel.name));
    send(call, std::format("disconnect {}", into_rest.name));
    if (!sibling) {
      send(call, std::format("disconnect {}", into_split.name));
      send(call, std::format("disconnect {}", into_target.name));
      reseat(call, into_split, split, into_rest.to.front());
      reseat(call,
             into_target,
             target + std::string(bar_node),
             {split, std::string(area_port)});
    }
    seat_halves(call, split, panel, target, side);
    const bool across = side == "left" || side == "right";
    send(call, std::format("param set {} axis {}", split, across ? "x" : "y"));
    send(call, std::format("param set {} ratio {}", split, even));
  }

  // A split's halves seat the panel and the target, the panel at the side given.
  void seat_halves(VP::Call &call,
                   const std::string &split,
                   const std::string &panel,
                   const std::string &target,
                   std::string_view side) {
    const bool first = side == "left" || side == "top";
    const std::array halves_of{first ? panel : target, first ? target : panel};
    for (std::size_t at = 0; at < halves_of.size(); ++at)
      connect(call,
              {.name = free_name(call, short_name(halves_of[at])),
               .from = {split, std::string(half_ports[at])},
               .to = {{halves_of[at] + std::string(bar_node), std::string(area_port)}}});
  }

  // The dock seating the panel goes, its other half taking its place.
  void leave(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    const std::string split = feeding(view, panel + std::string(bar_node))->from.node;
    const std::string_view half = feeding(view, panel + std::string(bar_node))->from.port;
    const VP::Connection *const rest =
        from(view, split, half == half_ports[0] ? half_ports[1] : half_ports[0]);
    const VP::Connection *const above = feeding(view, split);
    if (!rest || !above)
      throw std::runtime_error(
          std::format("{} seats {} alone, or has no Rect", split, panel));
    const VP::Connection seats = *above;
    const VP::Endpoint kept = rest->to.front();
    send(call, std::format("node remove {}", parent_of(split)));
    if (named(*call.view(_view), seats.name))
      send(call, std::format("disconnect {}", seats.name));
    reseat(call, seats, split, kept);
  }

  // The stack the panel holds goes, the content it hid taking its place in the panel;
  // the content it showed, which it returns, is left unconnected, for its new place.
  Content unstack(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    const std::string stack = stack_of(view, *content_of(view, panel));
    const auto node = std::ranges::find(view.nodes, stack, &VP::Node::name);
    const std::size_t shown = param_of(*node, "switch.shown") == half_ports[1] ? 1 : 0;
    const std::optional<Content> going = half_of(view, stack, shown);
    const std::optional<Content> staying = half_of(view, stack, 1 - shown);
    if (!going || !staying)
      throw std::runtime_error(std::format("{} holds no content in a half", stack));
    const VP::Connection room = *from(view, panel + std::string(bar_node), rest_port);
    const VP::Connection tabs =
        *into(view, panel + std::string(tabs_node), items_port);
    send(call, std::format("node remove {}", stack));
    connect(call, {.name = room.name, .from = room.from, .to = staying->area});
    const std::string named_tabs =
        std::format("{}-{}", content_name(*staying), tabs_port);
    connect(call,
            {.name = free_name(call, named_tabs), .from = staying->tabs, .to = tabs.to});
    return *going;
  }

  // A stack in the target's place holds what it held and the content, which shows.
  void join(VP::Call &call, const Content &content, const std::string &target) {
    const VP::View &view = *call.view(_view);
    const Content held = *content_of(view, target);
    const VP::Connection room = *from(view, target + std::string(bar_node), rest_port);
    const VP::Connection tabs = *into(view, target + std::string(tabs_node), items_port);
    const std::string stack =
        free_node(view, std::format("{}.{}", parent_of(target), stack_recipe));
    const std::string switcher = stack + std::string(switch_node);
    const std::string tabs_of = stack + std::string(switch_tabs_node);
    send(call,
         std::format("node add {} recipe={} param=switch.shown={}",
                     stack,
                     stack_recipe,
                     half_ports[1]));
    send(call, std::format("disconnect {}", room.name));
    send(call, std::format("disconnect {}", tabs.name));
    connect(call,
            {.name = room.name,
             .from = room.from,
             .to = {{switcher, std::string(area_port)}}});
    const std::array contents{held, content};
    for (std::size_t half = 0; half < contents.size(); ++half) {
      const std::string_view name = content_name(contents[half]);
      // The first half's room goes to the tabs too, by which they find the switch.
      VP::Connection room_of{.name = free_name(call, std::format("{}-area", name)),
                             .from = {switcher, std::string(half_ports[half])},
                             .to = contents[half].area};
      if (half == 0)
        room_of.to.push_back({tabs_of, std::string(half_ports[0])});
      connect(call, room_of);
      connect(call,
              {.name = free_name(call, std::format("{}-{}", name, tabs_port)),
               .from = contents[half].tabs,
               .to = {{tabs_of, std::format("{}-{}", half_ports[half], tabs_port)}}});
    }
    connect(call,
            {.name = free_name(call, std::format("{}-{}", short_name(stack), tabs_port)),
             .from = {tabs_of, std::string(tabs_port)},
             .to = tabs.to});
  }

  // A new panel holds the content, in a new dock in the target's place, both listed
  // before what draws over the panels, the dock before the target, so its ground draws
  // first; the panel takes what the one the content left takes, its theme, its font and
  // its atlas, but its room and its tabs, and the dock the theme that panel's ground
  // took.
  void seat_new(VP::Call &call,
                const Content &content,
                const std::string &source,
                const std::string &target,
                std::string_view side) {
    const VP::View &view = *call.view(_view);
    const std::string parent(parent_of(target));
    const std::string panel =
        free_node(view, std::format("{}.{}-panel", parent, content_name(content)));
    const std::string dock = free_node(view, std::format("{}.{}", parent, dock_recipe));
    const std::string own = source + ".";
    std::vector<VP::Connection> grown;
    for (const VP::Connection &connection : view.connections) {
      VP::Connection more = connection;
      for (const VP::Endpoint &to : connection.to) {
        const bool room = to.node == source + std::string(bar_node) && to.port == area_port;
        if (!to.node.starts_with(own) || room || to.port == items_port)
          continue;
        more.to.push_back({panel + to.node.substr(source.size()), to.port});
        if (to.node == own + "rects")
          more.to.push_back({dock + ".rects", to.port});
      }
      if (more.to.size() != connection.to.size())
        grown.push_back(std::move(more));
    }
    const VP::Connection seats = *feeding(view, target + std::string(bar_node));
    send(call, std::format("node add {} recipe={}", panel, panel_recipe));
    send(call, std::format("node move {} {}", panel, _name));
    send(call,
         std::format("node add {} recipe={} param=split.axis={} param=split.ratio={}",
                     dock,
                     dock_recipe,
                     side == "left" || side == "right" ? "x" : "y",
                     even));
    send(call, std::format("node move {} {}", dock, target));
    for (const VP::Connection &connection : grown) {
      send(call, std::format("disconnect {}", connection.name));
      connect(call, connection);
    }
    send(call, std::format("disconnect {}", seats.name));
    reseat(call,
           seats,
           target + std::string(bar_node),
           {dock + std::string(split_node), std::string(area_port)});
    seat_halves(call, dock + std::string(split_node), panel, target, side);
    const std::string_view name = content_name(content);
    connect(call,
            {.name = free_name(call, std::format("{}-area", name)),
             .from = {panel + std::string(bar_node), std::string(rest_port)},
             .to = content.area});
    connect(call,
            {.name = free_name(call, std::format("{}-{}", name, tabs_port)),
             .from = content.tabs,
             .to = {{panel + std::string(tabs_node), std::string(items_port)},
                    {panel + std::string(hit_node), std::string(items_port)}}});
  }

  // What a connection fed, at the node given, it now feeds at the endpoint given, named
  // for what it now seats.
  void reseat(VP::Call &call,
              VP::Connection connection,
              std::string_view node,
              VP::Endpoint to) {
    std::ranges::replace_if(
        connection.to, [&](const VP::Endpoint &end) { return end.node == node; }, to);
    const std::string_view seated =
        std::string_view(to.node).substr(0, to.node.rfind('.'));
    connection.name = free_name(call, short_name(seated));
    connect(call, connection);
  }

  // The name wanted, or with a number after it while a connection has it.
  std::string free_name(VP::Call &call, std::string_view wanted) const {
    const VP::View &view = *call.view(_view);
    std::string name(wanted);
    for (std::size_t count = 2; named(view, name); ++count)
      name = std::format("{}-{}", wanted, count);
    return name;
  }

  void connect(VP::Call &call, const VP::Connection &connection) {
    std::string line =
        std::format("connect {} {}", connection.name, connection.from.text());
    for (const VP::Endpoint &to : connection.to)
      line += ' ' + to.text();
    send(call, line);
  }

  void send(VP::Call &call, std::string_view line) {
    call.commands().send(std::format("{}: {}", _view, line));
  }

  VP::Command _move;
  VP::Command _hide;
  VP::Command _show;
  VP_VIEW::Items *_buttons = nullptr;
  // A dock's ratio before it hid a half, by its split, so showing it gives it back.
  std::map<std::string, float, std::less<>> _ratios;
  std::string _name;
  std::string _view; // the one it runs in, whose docks it edits
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  std::vector<Seated> _seated; // as the docks lay them out now
  // The area and the font's cell the panels were seated for; none until a frame seats
  // them, and after a rebuild.
  struct Laid {
    glm::ivec2 offset;
    glm::uvec2 extent;
    glm::uvec2 cell;
    bool operator==(const Laid &) const = default;
  };
  std::optional<Laid> _laid_out;
  glm::vec2 _pointer{};
  glm::vec2 _from{};                   // where the press that grabbed a panel was
  std::optional<std::size_t> _grabbed; // the panel whose strip was pressed
  bool _dragging = false;              // it went far enough from the press to drag
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Docking>("Docking");
}
