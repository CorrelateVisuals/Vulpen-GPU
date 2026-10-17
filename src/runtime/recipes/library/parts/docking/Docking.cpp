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

// A connection named for what it seats, as the manifest's own are, past the last dot.
std::string_view short_name(std::string_view node) {
  return node.substr(node.rfind('.') + 1);
}

// The half of a panel's place a drop at a side gives the panel dropped.
VP_VIEW::Rect half_at(const VP_VIEW::Rect &area, std::string_view side) {
  const glm::length_t along = side == "left" || side == "right" ? 0 : 1;
  const Halves split = halves(area, along, even);
  return side == "left" || side == "top" ? split.first : split.second;
}

// Moves a panel of a dock beside another, at its left, right, top or bottom, as edits of
// the docks that seat them in its own view's manifest: the dock seating the panel moves
// with it, its other half taking its place, into the place of the panel dropped on, and
// holds the two, halved; one log group, which a save keeps and a replay rebuilds (V08). A
// drag of a panel's tab strip onto another panel sends it, the side the edge of that
// panel the pointer is nearest. It hides a panel and shows it again, as its dock's ratio
// gives its half no room and back, and hands on a button a panel, lit while it shows,
// whose press hides or shows it, as the POC's menubar cells did. It finds where each
// panel and its strip show as the splits and the bars lay them out, from the Rect its
// area gives, which its host also hands the outermost dock, and while a drag goes on it
// frames where a drop would put the panel dragged.
class Docking final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _move = node.command("panel move <node> <node> <value>",
                         "moves a panel beside another, at its left, right, top or "
                         "bottom, as edits of the docks that seat them");
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
      return std::pair(at, sides[std::ranges::min_element(near) - near.begin()]);
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
  // names that view. The dock seating the panel moves with it: its other half takes its
  // place, and it takes the target's place, its halves the two, so every dock stays
  // listed before the panels whose ground it draws first, and a connection is named for
  // what it seats, as the manifest's own are.
  void move(VP::Call &call) {
    const std::string panel(call.arguments()[0]);
    const std::string target(call.arguments()[1]);
    const std::string_view side = call.arguments()[2];
    if (std::ranges::find(sides, side) == sides.end())
      throw std::runtime_error(
          std::format("side {}: left, right, top or bottom of {}", side, target));
    const VP::View &view = *call.view(_view);
    const VP::Connection *const into = feeding(view, panel + std::string(bar_node));
    const VP::Connection *const beside = feeding(view, target + std::string(bar_node));
    if (panel == target || !into || !beside || !into->from.node.ends_with(split_node))
      throw std::runtime_error(std::format(
          "{} moves beside another panel a dock seats, not {}; ls lists the nodes",
          panel,
          target));
    const std::string split = into->from.node;
    const std::string_view other =
        into->from.port == half_ports[0] ? half_ports[1] : half_ports[0];
    const auto rest =
        std::ranges::find_if(view.connections, [&](const VP::Connection &connection) {
          return connection.from.node == split && connection.from.port == other;
        });
    const VP::Connection *const above = feeding(view, split);
    if (rest == view.connections.end() || !above)
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
    const bool first = side == "left" || side == "top";
    const std::array halves_of{first ? panel : target, first ? target : panel};
    for (std::size_t at = 0; at < halves_of.size(); ++at)
      connect(call,
              {.name = free_name(call, short_name(halves_of[at])),
               .from = {split, std::string(half_ports[at])},
               .to = {{halves_of[at] + std::string(bar_node), std::string(area_port)}}});
    const bool across = side == "left" || side == "right";
    send(call, std::format("param set {} axis {}", split, across ? "x" : "y"));
    send(call, std::format("param set {} ratio {}", split, even));
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
