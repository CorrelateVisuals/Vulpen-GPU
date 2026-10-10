#include "Graph.h"

#include "runtime/View.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint32_t node_room = 256; // boxes shown at most; the rest are cut
constexpr std::uint32_t dot_room = 2048; // dots of the ground
constexpr std::uint32_t box_rects = 2;   // a box is its border, then its fill
constexpr std::uint32_t rect_room = dot_room + box_rects * node_room;
constexpr std::uint32_t character_room = 8192; // the names together
constexpr std::uint32_t curve_room = 512;      // curves, a lane taking two more
constexpr std::uint32_t lane_room = 256;       // rows connections keep in columns
constexpr std::int32_t pad = 4;           // in pixels, above and below a name in its box
constexpr std::int32_t border_width = 1;  // in pixels
constexpr std::int32_t dot_step = 24;     // between the dots of the ground, in pixels
constexpr std::uint32_t dot_size = 2;     // in pixels
constexpr std::int32_t column_gap = 4;    // in cells, where the curves run
constexpr std::int32_t name_margin = 1;   // in cells, either side of a name in its box
constexpr float wheel_step = 24.0f;       // pixels the graph moves a step of the wheel
constexpr std::string_view title = "graph";

// The node named, or the one a recipe it is inside makes it part of; none, past the
// last, for a name no node holds.
std::size_t node_of(const std::vector<VP::Node> &nodes, std::string_view name) {
  for (;;) {
    const auto found = std::ranges::find(nodes, name, &VP::Node::name);
    if (found != nodes.end())
      return static_cast<std::size_t>(found - nodes.begin());
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
      return nodes.size();
    name = name.substr(0, dot);
  }
}

// A node's folder in its view's (RV08): ui.panel is ui/panel/.
std::filesystem::path folder_of(const VP::View &view, std::string_view name) {
  std::filesystem::path folder = view.file.parent_path();
  for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
       dot = name.find('.')) {
    folder /= name.substr(0, dot);
    name.remove_prefix(dot + 1);
  }
  return folder / name;
}

// What of a Rect lies inside another; no extent when nothing does.
VP_VIEW::Rect cut(const VP_VIEW::Rect &rect, const VP_VIEW::Rect &by) {
  const glm::ivec2 to = rect.offset + glm::ivec2(rect.extent);
  const glm::ivec2 by_to = by.offset + glm::ivec2(by.extent);
  const glm::ivec2 from{std::max(rect.offset.x, by.offset.x),
                        std::max(rect.offset.y, by.offset.y)};
  const glm::ivec2 end{std::min(to.x, by_to.x), std::min(to.y, by_to.y)};
  if (end.x <= from.x || end.y <= from.y)
    return {.offset = from, .role = rect.role};
  return {.offset = from, .extent = glm::uvec2(end - from), .role = rect.role};
}

bool inside(const VP_VIEW::Rect &area, glm::vec2 point) {
  return VP_VIEW::contains(area, point);
}

} // namespace

void Graph::bind(VP::Bind &node) {
  _area = &node.input<VP_VIEW::Rect>("area");
  _font = &node.input<VP_VIEW::Font>("font");
  _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
  _labels = node.upload<VP_VIEW::Label>("labels", node_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  _curves = node.upload<VP_VIEW::Curve>("curves", curve_room);
  _tabs = &node.output<VP_VIEW::Items>("tabs");
  _placed.reserve(node_room); // so a frame never grows them (CPP10)
  _lefts.reserve(node_room);
  _downs.reserve(node_room);
  _ends.reserve(2 * curve_room);
  _lanes.reserve(lane_room);
}

// The view the terminal's lines go to: the one its view hosted last.
void Graph::cook(VP::Cook &frame) {
  const std::vector<VP::Child> &children = frame.view().children;
  const VP::View *const view =
      children.empty() ? nullptr : frame.hosted(children.back().name);
  const std::string_view shown = view ? std::string_view(children.back().name) : "";
  if (_tabs->empty() || shown != _shown)
    name_tab(shown);
  const glm::uvec2 cell = _font->cell;
  const bool room = view && _area->extent.x != 0 && _area->extent.y != 0 &&
                    cell.x != 0 && cell.y != 0;
  _placed.clear();
  if (room)
    lay_out(*view, cell);
  for (const VP::Event &event : frame.input().events())
    follow(frame, room ? view : nullptr, event);
  if (room) {
    write(frame, *view, cell);
    return;
  }
  // No room, no work: nothing written draws nothing.
  frame.write(_rects, 0);
  frame.write(_labels, 0);
  frame.write(_characters, 0);
  frame.write(_curves, 0);
}

// A box as wide as its name with a cell either side, and as tall as a row of text with
// pad above and below it, a column past every node it reads from.
void Graph::lay_out(const VP::View &view, glm::uvec2 cell) {
  const std::vector<VP::Node> &nodes = view.nodes;
  const std::size_t count = std::min(nodes.size(), std::size_t{node_room});
  for (std::size_t at = 0; at < count; ++at)
    _placed.push_back(
        {.extent = {static_cast<std::uint32_t>(nodes[at].name.size() + 2 * name_margin) *
                        cell.x,
                    cell.y + 2 * pad}});
  _ends.clear();
  for (const VP::Connection &connection : view.connections) {
    if (connection.from.view())
      continue;
    const std::size_t writer = node_of(nodes, connection.from.node);
    for (const VP::Endpoint &to : connection.to)
      if (const std::size_t reader = node_of(nodes, to.node);
          writer < count && reader < count && writer != reader &&
          _ends.size() < 2 * curve_room) {
        _ends.push_back(writer);
        _ends.push_back(reader);
      }
  }
  // The connections form no cycle, so as many rounds as boxes settle every column.
  for (std::size_t round = 0; round < count; ++round) {
    bool moved = false;
    for (std::size_t at = 0; at < _ends.size(); at += 2) {
      const std::size_t column = _placed[_ends[at]].column + 1;
      if (_placed[_ends[at + 1]].column < column) {
        _placed[_ends[at + 1]].column = column;
        moved = true;
      }
    }
    if (!moved)
      break;
  }
  _lanes.clear();
  for (std::size_t at = 0; at < _ends.size(); at += 2)
    for (std::size_t column = _placed[_ends[at]].column + 1;
         column < _placed[_ends[at + 1]].column && _lanes.size() < lane_room;
         ++column)
      _lanes.push_back({.pair = at / 2, .column = column});
  place_columns(cell);
}

// Each column as wide as its widest box and the gap after it, and its boxes a row each
// from its top, a row of text apart, then its lanes, a row each.
void Graph::place_columns(glm::uvec2 cell) {
  std::size_t columns = 0;
  for (const Placed &placed : _placed)
    columns = std::max(columns, placed.column + 1);
  _lefts.assign(columns, 0);
  _downs.assign(columns, static_cast<std::int32_t>(cell.y));
  for (const Placed &placed : _placed)
    _lefts[placed.column] =
        std::max(_lefts[placed.column], static_cast<std::int32_t>(placed.extent.x));
  const auto gap = static_cast<std::int32_t>(column_gap * cell.x);
  std::int32_t left = gap / 2;
  for (std::int32_t &width : _lefts)
    left += std::exchange(width, left) + gap;
  const auto row = static_cast<std::int32_t>(2 * cell.y + 2 * pad);
  for (Placed &placed : _placed) {
    placed.offset = {_lefts[placed.column], _downs[placed.column]};
    _downs[placed.column] += row;
  }
  // A lane's column is never the last, as a connection passing it reads from one beyond.
  for (Lane &lane : _lanes) {
    lane.offset = {_lefts[lane.column], _downs[lane.column]};
    lane.width = _lefts[lane.column + 1] - _lefts[lane.column] - gap;
    _downs[lane.column] += row;
  }
}

// In the order the events came, so a press is tested where the pointer was then. With
// no view in its area it takes no press and no wheel, only where the pointer went.
void Graph::follow(VP::Cook &frame, const VP::View *view, const VP::Event &event) {
  if (event.kind == VP::Event::Kind::pointer) {
    if (_dragging)
      _pan += event.at - _pointer;
    _pointer = event.at;
  } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
    if (!event.down)
      _dragging = false;
    else if (view && inside(*_area, _pointer))
      press(frame, *view);
  } else if (event.kind == VP::Event::Kind::wheel && view && inside(*_area, _pointer)) {
    _pan += event.turn * wheel_step;
  }
}

// On a box, it opens the node's first file, as a line typed would; on the ground, it
// grabs the graph, which follows the pointer until the button is let go.
void Graph::press(VP::Cook &frame, const VP::View &view) {
  for (std::size_t at = 0; at < _placed.size(); ++at) {
    if (!VP_VIEW::contains(cut(box(_placed[at]), *_area), _pointer))
      continue;
    const VP::Node &node = view.nodes[at];
    if (!node.files.empty())
      frame.commands().send(std::format(
          "open {}", (folder_of(view, node.name) / node.files.front()).generic_string()));
    return;
  }
  _dragging = true;
}

// The dots first, then a border and a fill a box, so the boxes draw over the ground.
void Graph::write(VP::Cook &frame, const VP::View &view, glm::uvec2 cell) {
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  std::uint32_t written = write_dots(rects);
  for (const Placed &placed : _placed) {
    const VP_VIEW::Rect outer = box(placed);
    const VP_VIEW::Rect border = cut(
        {.offset = outer.offset, .extent = outer.extent, .role = VP_VIEW::role("border")},
        *_area);
    if (border.extent.x == 0)
      continue;
    rects[written++] = border;
    rects[written++] = cut({.offset = outer.offset + border_width,
                            .extent = outer.extent - glm::uvec2(2 * border_width),
                            .role = VP_VIEW::role("panel")},
                           *_area);
  }
  frame.write(_rects, written); // the used length, which rects draws
  write_labels(frame, view, cell);
  write_curves(frame);
}

// A dot each step from where the ground moved to, inside the area.
std::uint32_t Graph::write_dots(std::span<VP_VIEW::Rect> rects) const {
  const VP_VIEW::Rect &area = *_area;
  const glm::ivec2 shift(_pan);
  const auto first = [&](std::int32_t from, std::int32_t moved) {
    return from + ((moved % dot_step) + dot_step) % dot_step;
  };
  std::uint32_t written = 0;
  const glm::ivec2 end = area.offset + glm::ivec2(area.extent);
  for (std::int32_t y = first(area.offset.y, shift.y); y < end.y; y += dot_step)
    for (std::int32_t x = first(area.offset.x, shift.x); x < end.x; x += dot_step) {
      if (written == dot_room)
        return written;
      rects[written++] = cut({.offset = {x, y},
                              .extent = glm::uvec2(dot_size),
                              .role = VP_VIEW::role("border")},
                             area);
    }
  return written;
}

// Each name a cell in from its box's left, between its pads; the characters left of the
// area are dropped, and its room ends at the area's right, which glyphs cuts at. A name
// whose row the area does not hold whole is not written.
void Graph::write_labels(VP::Cook &frame, const VP::View &view, glm::uvec2 cell) {
  const VP_VIEW::Rect &area = *_area;
  const std::span<VP_VIEW::Label> labels = frame.write(_labels);
  const std::span<VP_VIEW::Character> characters = frame.write(_characters);
  std::uint32_t written = 0;
  std::uint32_t next = 0;
  const std::int32_t right = area.offset.x + static_cast<std::int32_t>(area.extent.x);
  for (std::size_t at = 0; at < _placed.size(); ++at) {
    const std::string_view name = view.nodes[at].name;
    glm::ivec2 text = box(_placed[at]).offset +
                      glm::ivec2(static_cast<std::int32_t>(name_margin * cell.x), pad);
    if (text.y < area.offset.y ||
        text.y + static_cast<std::int32_t>(cell.y) >
            area.offset.y + static_cast<std::int32_t>(area.extent.y))
      continue;
    std::size_t skip = 0;
    if (text.x < area.offset.x) {
      skip = static_cast<std::size_t>(area.offset.x - text.x + cell.x - 1) / cell.x;
      text.x += static_cast<std::int32_t>(skip * cell.x);
    }
    if (skip >= name.size() || text.x >= right)
      continue;
    const auto shown = static_cast<std::uint32_t>(
        std::min(name.size() - skip, std::size_t{character_room - next}));
    for (std::uint32_t character = 0; character < shown; ++character)
      characters[next + character] = {
          .code = static_cast<unsigned char>(name[skip + character]), .label = written};
    labels[written++] = {.offset = text,
                         .extent = {static_cast<std::uint32_t>(right - text.x), cell.y},
                         .role = VP_VIEW::role("text"),
                         .first = next,
                         .count = shown};
    next += shown;
  }
  frame.write(_labels, written);
  frame.write(_characters, next);
}

// From the middle of the writer's right side to the middle of the reader's left, along
// each lane it keeps on the way, a curve to the lane and one along it.
void Graph::write_curves(VP::Cook &frame) {
  const std::span<VP_VIEW::Curve> curves = frame.write(_curves);
  const glm::ivec2 origin = _area->offset + glm::ivec2(_pan);
  const auto middle = [](const VP_VIEW::Rect &rect, bool right) {
    const auto across = right ? static_cast<std::int32_t>(rect.extent.x) : 0;
    return glm::vec2(rect.offset.x + across,
                     rect.offset.y + static_cast<std::int32_t>(rect.extent.y / 2));
  };
  const auto height = static_cast<std::int32_t>(
      _placed.empty() ? 0 : _placed.front().extent.y / 2);
  std::uint32_t written = 0;
  std::size_t lane = 0;
  for (std::size_t at = 0; at < _ends.size(); at += 2) {
    glm::vec2 from = middle(box(_placed[_ends[at]]), true);
    for (; lane < _lanes.size() && _lanes[lane].pair == at / 2; ++lane) {
      const glm::vec2 left(origin + _lanes[lane].offset + glm::ivec2(0, height));
      const glm::vec2 right = left + glm::vec2(_lanes[lane].width, 0);
      write_curve(curves, written, from, left);
      write_curve(curves, written, left, right);
      from = right;
    }
    write_curve(curves, written, from, middle(box(_placed[_ends[at + 1]]), false));
  }
  frame.write(_curves, written);
}

// When both its ends lie inside the area: a curve's bend stays between its ends, so it
// does too.
void Graph::write_curve(std::span<VP_VIEW::Curve> curves,
                        std::uint32_t &written,
                        glm::vec2 from,
                        glm::vec2 to) const {
  if (written < curves.size() && inside(*_area, from) && inside(*_area, to))
    curves[written++] = {.from = from, .to = to, .role = VP_VIEW::role("accent")};
}

// Its title, and the view it shows, as the terminal's names the view its lines go to.
void Graph::name_tab(std::string_view shown) {
  _shown = shown;
  _tabs->assign(1,
                {.label = shown.empty() ? std::string(title)
                                        : std::format("{} - {}", title, shown),
                 .current = true});
}

VP_VIEW::Rect Graph::box(const Placed &placed) const {
  return {.offset = _area->offset + placed.offset + glm::ivec2(_pan),
          .extent = placed.extent};
}

VP_OPERATORS(registry) {
  registry.add<Graph>("Graph");
}
