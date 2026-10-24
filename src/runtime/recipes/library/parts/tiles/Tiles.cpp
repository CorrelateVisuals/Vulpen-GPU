#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Tiling.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Halves.h"
#include "shared/Presented.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Its own ports, beside those its host connects to the tiles, each of which shows a
// project: the port a tile's images reach that project's window at.
constexpr std::string_view rects_port = "rects";
constexpr std::string_view tiling_port = "tiling";
constexpr std::string_view picture_port = "picture";
constexpr std::string_view typed_port = "typed"; // the keys part's, for the focus
// The connection bringing a project's window to a tile while that project is not the
// one presented, named for the tile's port.
constexpr std::string_view window_prefix = "window-";
constexpr std::string_view middle = "middle";
// The frame round the tile of the project presented, while several show.
constexpr std::int32_t frame_width = 1;
constexpr std::uint32_t frame_rects = 4;

// Shows the projects a GUI hosts side by side in the Rect its area gives: those its shown
// param names, in order, along its axis, x or y, each an equal share, or, while it names
// fewer than two hosted, the one presented alone. Its host connects each port but rects
// and tiling to the images of a tile, and each takes the window of the project its tile
// shows at its picture: the project presented through the connection shared/Presented.h
// names, which the ports its to param names take too, as the image filling the window in
// perform mode, and the others through one a tile, so a window reaches the images
// showing it through one connection, as a port joins one. A project keeps its tile while
// hosted, so what the tile holds, as a frame held still and where it is moved and
// zoomed, waits while another shows, and a tile is taken from the project shown longest
// ago only when none is free; a tile showing none has no room, and its project's window,
// but the one presented, draws nowhere. It presents the project the current view is, or
// holds; a project presented that no tile shows takes the place of the one presented
// before, once the press that may have presented it is let go, so a tab pressed shows its
// project there, and one dragged onto the panel's own edge goes beside the others, as
// the docking part asks through tiling, and in its middle shows alone again; a press in a
// tile presents its project. The focus on a tile giving its place to another, while its
// host connects the keys part's typed to its typed, goes to the same node of the other,
// so keys go to the project shown there. Its lines set its shown and axis params, so a
// save keeps the tiles and a replay restores them (V08). It draws a seam in the border's
// role between tiles, and round the one presented, while several show, a frame in the
// accent.
class Tiles final : public VP::Operator, public VP_VIEW::Tiling {
  // A tile: its port, the Rect it hands the nodes it reaches there, those nodes, in the
  // order the host lists them, and their pictures, each a blank before it, as a connect
  // line names them.
  struct Tile {
    std::string port;
    VP_VIEW::Rect *room = nullptr;
    std::vector<std::string> nodes;
    std::string takers;
  };

  void bind(VP::Bind &node) override {
    _name = node.name();
    _view = node.view_name();
    _to = node.sets("to") ? node.param<std::string>("to") : std::string();
    std::ranges::replace(_to, ',', ' ');
    const std::string axis = node.param<std::string>("axis");
    if (axis != "x" && axis != "y")
      throw std::runtime_error(
          std::format("param axis = {}: x, side by side, or y, stacked", axis));
    _along = axis == "y" ? 1 : 0;
    _shown.clear();
    const std::string shown = node.sets("shown") ? node.param<std::string>("shown") : "";
    for (const auto word : std::views::split(shown, ','))
      if (!std::ranges::empty(word))
        _shown.emplace_back(word.begin(), word.end());
    _area = &node.input<VP_VIEW::Rect>("area");
    node.output<VP_VIEW::Tiled>(tiling_port).tiling = this;
    _typed = nullptr;
    _tiles.clear();
    for (const VP::Connection &connection : node.view().connections) {
      if (std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
            return to.node == _name && to.port == typed_port;
          }))
        _typed = &node.input<VP_VIEW::Typed>(typed_port);
      const std::string_view port = connection.from.port;
      if (connection.from.node != _name || port == rects_port || port == tiling_port)
        continue;
      Tile tile{.port = std::string(port), .room = &node.output<VP_VIEW::Rect>(port)};
      for (const VP::Endpoint &to : connection.to) {
        tile.nodes.push_back(to.node);
        tile.takers += std::format(" {}.{}", to.node, picture_port);
      }
      _tiles.push_back(std::move(tile));
    }
    _rects = node.upload<VP_VIEW::Rect>(
        rects_port, static_cast<std::uint32_t>(std::max<std::size_t>(_tiles.size(), 1)) +
                        frame_rects);
    _chosen = false; // the params may show others now
  }

  // The views are read before a line goes, as each replaces them.
  void cook(VP::Cook &frame) override {
    ++_frame;
    const VP::View &top = *frame.view("");
    const std::string_view presented = shown_by(frame.view());
    const std::string_view now =
        wanted([&](std::string_view name) { return frame.view(name); },
               top,
               _view,
               frame.commands().current(),
               presented,
               _started);
    _started = true;
    if (now != _selected) {
      _previous = std::exchange(_selected, std::string(now));
      _pending = true;
      _chosen = false;
    }
    auto hosted = projects(top, _view);
    if (!std::ranges::equal(_hosted, hosted, {}, {}, &VP::Child::name)) {
      _hosted.clear();
      for (const VP::Child &child : hosted)
        _hosted.push_back(child.name);
      _chosen = false;
    }
    follow(frame);
    if (_pending && !_down && _frame > _let_go) {
      _pending = false;
      if (!std::exchange(_decided, false))
        take_place(frame.commands());
    }
    if (!std::exchange(_chosen, true)) {
      choose();
      assign();
      connect(frame.commands(), frame.view());
      refocus(frame.commands());
    }
    lay_out(frame);
  }

  // A press in a tile presents its project; a press anywhere holds the change of the one
  // presented until it is let go, and a frame more, so a drag from a tab onto the panel's
  // own edge, which the docking part sends as it is let go, decides where it goes.
  void follow(VP::Cook &frame) {
    for (const VP::Event &event : frame.input().events()) {
      if (event.kind == VP::Event::Kind::pointer) {
        _pointer = event.at;
      } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
        _down = event.down;
        if (!event.down)
          _let_go = _frame;
        else if (const std::optional<std::size_t> at = under();
                 at && _visible[*at] != _selected)
          frame.commands().send(std::format("view switch {}", _visible[*at]));
      }
    }
  }

  // The tile shown under the pointer, as the frame before laid them out; none for none.
  std::optional<std::size_t> under() const {
    for (std::size_t at = 0; at < _rooms.size(); ++at)
      if (VP_VIEW::contains(_rooms[at], _pointer))
        return at;
    return std::nullopt;
  }

  // A project presented that no tile shows takes the place of the one presented before
  // it, or else of the last, so the tab pressed shows its project where the other was.
  void take_place(VP::CommandPort &commands) const {
    if (_visible.size() < 2 || _selected.empty() ||
        std::ranges::find(_visible, _selected) != _visible.end())
      return;
    std::vector<std::string> next = _visible;
    const auto place = std::ranges::find(next, _previous);
    (place == next.end() ? next.back() : *place) = _selected;
    set_shown(commands, next);
  }

  void set_shown(VP::CommandPort &commands, std::span<const std::string> shown) const {
    std::string joined;
    for (const std::string &name : shown)
      joined += (joined.empty() ? "" : ",") + name;
    commands.send(std::format("{}: param set {} shown {}", _view, _name, joined));
  }

  bool hosts(std::string_view name) const {
    return std::ranges::find(_hosted, name) != _hosted.end();
  }

  // Those the shown param names that are hosted, in order, while two or more are, as many
  // as there are tiles; else the one presented alone.
  void choose() {
    _visible.clear();
    for (const std::string &name : _shown)
      if (hosts(name) && std::ranges::find(_visible, name) == _visible.end() &&
          _visible.size() < _tiles.size())
        _visible.push_back(name);
    if (_visible.size() < 2) {
      _visible.clear();
      if (!_selected.empty() && !_tiles.empty())
        _visible.push_back(_selected);
    }
  }

  // Each project shown keeps the tile it holds, or takes a free one, a tile being free
  // once its project closed, or else the one whose project was shown longest ago.
  void assign() {
    std::erase_if(_held, [&](const auto &held) {
      return !hosts(held.second) || std::ranges::none_of(_tiles, [&](const Tile &tile) {
        return tile.port == held.first;
      });
    });
    for (const std::string &project : _visible) {
      if (const Tile *const tile = tile_of(project)) {
        _seen[tile->port] = _frame;
        continue;
      }
      const Tile *taken = nullptr;
      for (const Tile &tile : _tiles) {
        const auto held = _held.find(tile.port);
        if (held == _held.end()) {
          taken = &tile;
          break;
        }
        if (std::ranges::find(_visible, held->second) == _visible.end() &&
            (!taken || _seen[tile.port] < _seen[taken->port]))
          taken = &tile;
      }
      if (!taken)
        continue; // more shown than tiles, which choose never lets happen
      _held[taken->port] = project;
      _seen[taken->port] = _frame;
    }
  }

  const Tile *tile_of(std::string_view project) const {
    const auto found = std::ranges::find_if(_tiles, [&](const Tile &tile) {
      const auto held = _held.find(tile.port);
      return held != _held.end() && held->second == project;
    });
    return found == _tiles.end() ? nullptr : &*found;
  }

  // The focus on a tile that gave its place to another goes to the node of the other
  // listed where it was listed in its own, as the image of one to the image of the other.
  void refocus(VP::CommandPort &commands) {
    const std::size_t places = _typed ? std::min(_visible.size(), _placed.size()) : 0;
    for (std::size_t at = 0; at < places; ++at) {
      const Tile *const now = tile_of(_visible[at]);
      const auto was = std::ranges::find(_tiles, _placed[at], &Tile::port);
      if (!now || was == _tiles.end() || &*was == now)
        continue;
      const auto focus = std::ranges::find(was->nodes, _typed->focus);
      const auto listed = static_cast<std::size_t>(focus - was->nodes.begin());
      if (focus != was->nodes.end() && listed < now->nodes.size())
        commands.send(std::format("{}: focus {}", _view, now->nodes[listed]));
    }
    _placed.clear();
    for (const std::string &project : _visible)
      if (const Tile *const tile = tile_of(project))
        _placed.push_back(tile->port);
  }

  bool shows(std::string_view project) const {
    return std::ranges::find(_visible, project) != _visible.end();
  }

  // The windows to the tiles showing them, and the one presented to its to param's ports
  // too: a line only for a connection that differs from the one there, those going first,
  // as a window joins one connection at a time.
  void connect(VP::CommandPort &commands, const VP::View &own) {
    struct Wanted {
      std::string name;
      std::string from;
      std::string to;
    };
    std::vector<Wanted> lines;
    if (!_selected.empty()) {
      const Tile *const tile = shows(_selected) ? tile_of(_selected) : nullptr;
      std::string to = _to + (tile ? tile->takers : std::string());
      if (!to.empty())
        lines.push_back({std::string(presenting), _selected, std::move(to)});
    }
    for (const std::string &project : _visible)
      if (const Tile *const tile = tile_of(project); tile && project != _selected)
        lines.push_back(
            {std::string(window_prefix) + tile->port, project, tile->takers});
    std::vector<std::string> going;
    for (const VP::Connection &connection : own.connections) {
      if (connection.name != presenting && !connection.name.starts_with(window_prefix))
        continue;
      std::string to;
      for (const VP::Endpoint &end : connection.to)
        to += " " + end.text();
      const auto kept = std::ranges::find_if(lines, [&](const Wanted &line) {
        return line.name == connection.name && connection.from.view() &&
               line.from == connection.from.node && words(line.to) == words(to);
      });
      if (kept == lines.end())
        going.push_back(connection.name);
      else
        lines.erase(kept);
    }
    for (const std::string &name : going)
      commands.send(std::format("{}: disconnect {}", _view, name));
    for (const Wanted &line : lines)
      commands.send(
          std::format("{}: connect {} {}: {}", _view, line.name, line.from, line.to));
  }

  // Ports joined by blanks, as the same ports whatever blanks lead them.
  static std::string_view words(std::string_view text) {
    const std::size_t start = text.find_first_not_of(' ');
    return start == std::string_view::npos ? std::string_view() : text.substr(start);
  }

  // Along its axis, each tile shown an equal share of what the seams between them leave,
  // and the others no room; a seam between each two, and a frame round the one presented,
  // while several show.
  void lay_out(VP::Cook &frame) {
    for (const Tile &tile : _tiles)
      *tile.room = {};
    const auto count = static_cast<std::int32_t>(_visible.size());
    if (_rooms.size() != _visible.size())
      _rooms.resize(_visible.size());
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
    std::uint32_t written = 0;
    const auto whole = static_cast<std::int32_t>(_area->extent[_along]);
    const std::int32_t room = std::max(0, whole - seam_width * std::max(0, count - 1));
    for (std::int32_t at = 0; at < count; ++at) {
      const std::int32_t start = room * at / count + seam_width * at;
      const std::int32_t length = room * (at + 1) / count - room * at / count;
      VP_VIEW::Rect &placed = _rooms[static_cast<std::size_t>(at)];
      placed = part_of(*_area, _along, start, length);
      if (const Tile *const tile = tile_of(_visible[static_cast<std::size_t>(at)]))
        *tile->room = placed;
      if (at + 1 < count) {
        const VP_VIEW::Rect seam = part_of(*_area, _along, start + length, seam_width);
        rects[written++] = {.offset = seam.offset,
                            .extent = seam.extent,
                            .role = VP_VIEW::role("border")};
      }
    }
    const auto presented = std::ranges::find(_visible, _selected);
    if (count >= 2 && presented != _visible.end())
      written += ring(rects.subspan(written),
                      _rooms[static_cast<std::size_t>(presented - _visible.begin())]);
    frame.write(_rects, written);
  }

  // A frame a pixel wide inside a Rect, in the accent. Returns how many Rects it took.
  static std::uint32_t ring(std::span<VP_VIEW::Rect> rects, const VP_VIEW::Rect &at) {
    const auto wide = static_cast<std::uint32_t>(frame_width);
    const glm::ivec2 far = at.offset + glm::ivec2(at.extent) - frame_width;
    rects[0] = {.offset = at.offset, .extent = {at.extent.x, wide}};
    rects[1] = {.offset = {at.offset.x, far.y}, .extent = {at.extent.x, wide}};
    rects[2] = {.offset = at.offset, .extent = {wide, at.extent.y}};
    rects[3] = {.offset = {far.x, at.offset.y}, .extent = {wide, at.extent.y}};
    for (VP_VIEW::Rect &edge : rects.first(frame_rects))
      edge.role = VP_VIEW::role("accent");
    return frame_rects;
  }

  const std::string &name() const override {
    return _name;
  }

  bool more() const override {
    return _hosted.size() >= 2;
  }

  bool several() const override {
    return _visible.size() >= 2;
  }

  // The one presented, which the press on its tab presented, goes beside those shown, or
  // beside the one presented before it while it showed alone, at the side named; in the
  // middle it shows alone again.
  void divide(VP::Call &call, std::string_view side) override {
    _decided = true;
    if (_selected.empty())
      throw std::runtime_error(std::format("{} presents no project", _name));
    if (side == middle) {
      set_shown(call.commands(), std::array{_selected});
      return;
    }
    std::vector<std::string> next = several() ? _visible : std::vector<std::string>();
    std::erase(next, _selected);
    if (next.empty()) {
      const auto other = std::ranges::find_if(_hosted, [&](const std::string &name) {
        return name != _selected;
      });
      if (other == _hosted.end())
        throw std::runtime_error(std::format(
            "{} hosts no project but {}, so none goes beside it", _name, _selected));
      next.push_back(hosts(_previous) && _previous != _selected ? _previous : *other);
    }
    if (side == "left" || side == "top")
      next.insert(next.begin(), _selected);
    else
      next.push_back(_selected);
    if (next.size() > _tiles.size())
      throw std::runtime_error(
          std::format("{} shows {} side by side at most, as it has as many tiles; a tab "
                      "dropped in its middle shows one again",
                      _name,
                      _tiles.size()));
    call.commands().send(std::format("{}: param set {} axis {}",
                                     _view,
                                     _name,
                                     side == "left" || side == "right" ? "x" : "y"));
    set_shown(call.commands(), next);
  }

  std::string _name;
  std::string _view; // the one it runs in, which its lines change
  std::string _to;   // the ports the window presented reaches too, blank between
  glm::length_t _along = 0;
  std::vector<std::string> _shown; // as its param names them
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Typed *_typed = nullptr; // while its host connects it
  VP::Upload<VP_VIEW::Rect> _rects;
  std::vector<Tile> _tiles;
  std::vector<std::string> _placed; // the port of the tile shown at each place
  // The project each tile's port holds, and the frame it last showed in it, kept across a
  // rebind, so a project keeps its tile.
  std::map<std::string, std::string, std::less<>> _held;
  std::map<std::string, std::uint64_t, std::less<>> _seen;
  std::vector<std::string> _hosted;  // the projects, as the frame before found them
  std::vector<std::string> _visible; // those shown, in order
  std::vector<VP_VIEW::Rect> _rooms; // where each shows, as laid out last
  std::string _selected;             // the project presented, or about to be
  std::string _previous;             // the one before it
  bool _chosen = false;  // the projects shown, their tiles and windows follow what is
  bool _started = false; // after the first frame, which may present the one hosted last
  bool _pending = false; // the one presented changed, and no tile shows it yet
  bool _decided = false; // a drop on the panel's edge or middle placed it since
  bool _down = false;    // the left button, as the events so far left it
  std::uint64_t _frame = 0;
  std::uint64_t _let_go = 0; // the frame it was let go in
  glm::vec2 _pointer{};
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Tiles>("Tiles");
}
