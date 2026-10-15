#include "Graph.h"

#include "runtime/View.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint32_t node_room = 256; // boxes shown at most; the rest are cut
constexpr std::uint32_t box_rects = 2;   // a box is its border, then its fill
constexpr std::uint32_t rect_room = box_rects * node_room;
constexpr std::uint32_t label_room = 2048;      // the names and the files together
constexpr std::uint32_t character_room = 16384; // their characters
constexpr std::uint32_t curve_room = 512;       // curves, a lane taking two more
constexpr std::uint32_t lane_room = 256;        // rows connections keep in columns
constexpr std::int32_t pad = 4;                 // in pixels, above and below a box's rows
constexpr std::int32_t border_width = 1;        // in pixels
constexpr std::int32_t selected_width = 2;      // a selected box's border, in pixels
constexpr std::int32_t column_gap = 4;          // in cells, where the curves run
constexpr std::int32_t name_margin = 1; // in cells, either side of a row in its box
constexpr float drag_least = 3.0f;         // pixels a press moves before a box follows
constexpr std::uint64_t quiet_frames = 30; // after the wheel stops, before a save
constexpr std::string_view title = "graph";
constexpr std::string_view kept_folder = ".vulpen"; // which no build or scan reads
constexpr std::string_view kept_file = "graph.ini";
constexpr std::string_view place_key = "place ";
constexpr std::string_view blanks = " \t\r";
constexpr std::string_view kept_header =
    "# The graph editor's look at this view, which a restart keeps: how far the graph\n"
    "# moved, how near it is, and each box placed by hand, from the graph's top left\n"
    "# at zoom 1. The graph part writes it as they change; delete it to lay the boxes\n"
    "# out from the graph again.\n";

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

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The numbers a value holds, between blanks; none when a word is no number.
std::vector<float> numbers_of(std::string_view text) {
  std::vector<float> numbers;
  for (text = trim(text); !text.empty(); text = trim(text)) {
    const std::size_t length = std::min(text.find_first_of(blanks), text.size());
    float number = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + length, number);
    if (error != std::errc{} || end != text.data() + length)
      return {};
    numbers.push_back(number);
    text.remove_prefix(length);
  }
  return numbers;
}

// Each line as save writes it: pan = x y, zoom = z, or place <node> = x y; # starts a
// comment. Throws naming the first line that is none of them (A02).
Arranged arranged_of(std::string_view text) {
  Arranged arranged;
  for (std::size_t line = 1; !text.empty(); ++line) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view row = trim(text.substr(0, std::min(text.find('#'), end)));
    text.remove_prefix(std::min(end + 1, text.size()));
    if (row.empty())
      continue;
    const std::size_t equals = std::min(row.find('='), row.size());
    const std::string_view key = trim(row.substr(0, equals));
    const std::vector<float> numbers =
        numbers_of(row.substr(std::min(equals + 1, row.size())));
    if (key == "pan" && numbers.size() == 2)
      arranged.pan = {numbers[0], numbers[1]};
    else if (key == "zoom" && numbers.size() == 1 && numbers[0] >= PanZoom::least &&
             numbers[0] <= PanZoom::most)
      arranged.zoom = numbers[0];
    else if (key.starts_with(place_key) && numbers.size() == 2 &&
             !trim(key.substr(place_key.size())).empty())
      arranged.places.insert_or_assign(
          std::string(trim(key.substr(place_key.size()))),
          glm::ivec2(std::lround(numbers[0]), std::lround(numbers[1])));
    else
      throw std::runtime_error(std::format(
          "{}:{}: {} is none of pan = x y, zoom = z from {} to {}, place <node> = x y",
          kept_file,
          line,
          row,
          PanZoom::least,
          PanZoom::most));
  }
  return arranged;
}

} // namespace

void Graph::bind(VP::Bind &node) {
  _area = &node.input<VP_VIEW::Rect>("area");
  _font = &node.input<VP_VIEW::Font>("font");
  _typed = &node.input<VP_VIEW::Typed>("typed");
  _name = node.name();
  _select = node.command("select [<node>...]",
                         "selects nodes of the view the graph shows, in place of those "
                         "selected; with none, none");
  _selected =
      node.command("selected", "lists the nodes the graph has selected, a line each");
  _delete = node.command("delete",
                         "removes the nodes the graph has selected, the nodes inside "
                         "them and their connections; their folders stay");
  _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
  _labels = node.upload<VP_VIEW::Label>("labels", label_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  _curves = node.upload<VP_VIEW::Curve>("curves", curve_room);
  _tabs = &node.output<VP_VIEW::Items>("tabs");
  _ground = node.upload<VP_VIEW::Rect>("ground");
  _pan = node.value<glm::vec2>("pan");
  _zoom = node.value<float>("zoom");
  _placed.reserve(node_room); // so a frame never grows them (CPP10)
  _lefts.reserve(node_room);
  _downs.reserve(node_room);
  _ends.reserve(2 * curve_room);
  _lanes.reserve(lane_room);
}

// The view the terminal's lines go to: the one its view hosted last. The keys once it is
// known, and before it is read for the frame, as delete changes it.
void Graph::cook(VP::Cook &frame) {
  const std::vector<VP::Child> &children = frame.view().children;
  const VP::View *view = children.empty() ? nullptr : frame.hosted(children.back().name);
  const std::string_view shown = view ? std::string_view(children.back().name) : "";
  if (_tabs->empty() || shown != _shown)
    show(frame, view, shown);
  for (const VP::Event &event : _typed->to(_name))
    if (event.kind == VP::Event::Kind::key && event.down)
      key(frame, event.name);
  if (view)
    view = frame.hosted(shown);
  const glm::uvec2 cell = _font->cell;
  const bool room =
      view && _area->extent.x != 0 && _area->extent.y != 0 && cell.x != 0 && cell.y != 0;
  _placed.clear();
  if (room)
    lay_out(*view, cell);
  for (const VP::Event &event : frame.input().events())
    follow(frame, room ? view : nullptr, event, cell);
  // Once the wheel rests, so a turn of it writes the file once, not a step at a time.
  if (_unsaved && !_dragging && !_grab && frame.index() >= *_unsaved + quiet_frames)
    save(frame);
  frame.set(_pan, _arranged.pan);
  frame.set(_zoom, _arranged.zoom);
  if (room) {
    write(frame, *view, cell);
    return;
  }
  // No room, no work: nothing written draws nothing.
  frame.write(_ground, 0);
  frame.write(_rects, 0);
  frame.write(_labels, 0);
  frame.write(_characters, 0);
  frame.write(_curves, 0);
}

// On the view the graph shows, the one its view hosts last, as the terminal's lines go to
// it, so a line to any other is refused rather than acting where nobody looks. The view
// is found from the lines so far, so a script's select before the first frame finds it.
void Graph::command(VP::Call &call) {
  const std::vector<VP::Child> &children = call.view("")->children;
  const std::string shown = children.empty() ? "" : children.back().name;
  if (shown.empty() || call.view(shown) != &call.view())
    throw std::runtime_error(
        shown.empty() ? "the graph shows no view; view load hosts one"
                      : std::format("the graph shows {}, so its lines go there, as {}: "
                                    "select",
                                    shown,
                                    shown));
  if (_selected_in != shown) // a selection in a view hosted before
    _selection.clear();
  const std::vector<VP::Node> &nodes = call.view().nodes;
  if (call.is(_select)) {
    std::vector<std::string> chosen;
    for (const std::string_view node : call.arguments()) {
      if (std::ranges::find(nodes, node, &VP::Node::name) == nodes.end())
        throw std::runtime_error(std::format("no node is named {}; ls lists them", node));
      if (std::ranges::find(chosen, node) == chosen.end())
        chosen.emplace_back(node);
    }
    _selection = std::move(chosen);
    _selected_in = shown;
  } else if (call.is(_selected)) {
    for (const std::string &node : _selection)
      if (std::ranges::find(nodes, node, &VP::Node::name) != nodes.end())
        call.reply(node);
  } else if (call.is(_delete)) {
    erase(call, call.view());
  }
}

// delete or backspace deletes what is selected; escape lets go of it, and with none of
// the focus, as the editor and the terminal do. The home key brings the graph home, and
// saves it at once, as a drag does.
void Graph::key(VP::Cook &frame, std::string_view key) {
  const bool chosen = !_shown.empty() && _selected_in == _shown && !_selection.empty();
  if ((key == "delete" || key == "backspace") && chosen) {
    frame.commands().send(std::format("{}: delete", _shown));
  } else if (key == "escape" && chosen) {
    frame.commands().send(std::format("{}: select", _shown));
  } else if (key == "escape") {
    frame.commands().send("focus");
  } else if (key == PanZoom::home_key) {
    _arranged.home();
    save(frame);
  }
}

// The tab names the view, and what its .ini kept shows it as it was left. One that holds
// a mistake is named in the log and kept as it is, so no save writes over what a person
// typed; the graph then lays the view out from the graph alone. A selection in another
// view goes.
void Graph::show(VP::Cook &frame, const VP::View *view, std::string_view name) {
  if (_unsaved)
    save(frame);
  _shown = name;
  if (_selected_in != name)
    _selection.clear();
  _tabs->assign(
      1,
      {.label = name.empty() ? std::string(title) : std::format("{} - {}", title, name),
       .current = true});
  _arranged = {};
  _ini.clear();
  _refused = false;
  _grab.reset();
  if (!view)
    return;
  const std::filesystem::path folder = view->file.parent_path() / kept_folder;
  _ini = (folder / kept_file).string();
  const std::vector<std::string> kept = frame.files().list(folder.string());
  if (std::ranges::find(kept, kept_file) == kept.end())
    return;
  try {
    _arranged = arranged_of(frame.files().read(_ini));
  } catch (const std::runtime_error &mistake) {
    _arranged = {};
    _refused = true;
    frame.log(VP::Level::error,
              std::format("{}; the graph lays the view out from the graph alone, and "
                          "keeps the file as it is",
                          mistake.what()));
  }
}

// A box as wide as its widest row with a cell either side, and a row of text for its
// name and each file, with pad above and below them, a column past every node it reads
// from; a box placed by hand where it was put.
void Graph::lay_out(const VP::View &view, glm::uvec2 cell) {
  const std::vector<VP::Node> &nodes = view.nodes;
  const std::size_t count = std::min(nodes.size(), std::size_t{node_room});
  for (std::size_t at = 0; at < count; ++at) {
    std::size_t widest = nodes[at].name.size();
    for (const std::string &file : nodes[at].files)
      widest = std::max(widest, file.size());
    const auto rows = static_cast<std::uint32_t>(1 + nodes[at].files.size());
    _placed.push_back(
        {.extent = {static_cast<std::uint32_t>(widest + 2 * name_margin) * cell.x,
                    rows * cell.y + 2 * pad}});
  }
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
  // Lanes run past the columns, which boxes placed by hand leave behind.
  _lanes.clear();
  for (std::size_t at = 0; at < _ends.size() && _arranged.places.empty(); at += 2)
    for (std::size_t column = _placed[_ends[at]].column + 1;
         column < _placed[_ends[at + 1]].column && _lanes.size() < lane_room;
         ++column)
      _lanes.push_back({.pair = at / 2, .column = column});
  place_columns(cell);
  for (std::size_t at = 0; at < count; ++at)
    if (const auto place = _arranged.places.find(nodes[at].name);
        place != _arranged.places.end())
      _placed[at].offset = place->second;
}

// Each column as wide as its widest box and the gap after it, and its boxes one under the
// other from its top, a row of text apart, then its lanes, a row each.
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
  const auto apart = static_cast<std::int32_t>(cell.y);
  for (Placed &placed : _placed) {
    placed.offset = {_lefts[placed.column], _downs[placed.column]};
    _downs[placed.column] += static_cast<std::int32_t>(placed.extent.y) + apart;
  }
  // A lane's column is never the last, as a connection passing it reads from one beyond.
  const auto lane_row = static_cast<std::int32_t>(cell.y + 2 * pad) + apart;
  for (Lane &lane : _lanes) {
    lane.offset = {_lefts[lane.column], _downs[lane.column]};
    lane.width = _lefts[lane.column + 1] - _lefts[lane.column] - gap;
    _downs[lane.column] += lane_row;
  }
}

// In the order the events came, so a press is tested where the pointer was then. With
// no view in its area it takes no press and no wheel, only where the pointer went.
void Graph::follow(VP::Cook &frame,
                   const VP::View *view,
                   const VP::Event &event,
                   glm::uvec2 cell) {
  if (event.kind == VP::Event::Kind::pointer) {
    if (_grab && view) {
      drag(*view, event.at);
    } else if (_dragging) {
      _arranged.pan += event.at - _pointer;
      _unsaved = frame.index();
    }
    _pointer = event.at;
  } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
    if (!event.down)
      release(frame, view, cell);
    else if (view && inside(*_area, _pointer))
      press(frame, *view);
  } else if (event.kind == VP::Event::Kind::wheel && view && inside(*_area, _pointer)) {
    turn(frame, event.turn);
  } else if (event.kind == VP::Event::Kind::key && event.name == "shift") {
    _shift = event.down;
  } else if (event.kind == VP::Event::Kind::focus && !event.down) {
    // Let go while the window had no focus, which no event says.
    _shift = false;
  }
}

// It takes the focus. On a box, the box drawn last over the others: shift adds it to the
// selection or takes it out; else it is selected, unless it is already, and grabbed with
// every box selected, so a drag moves them all. On the ground, the graph, which follows
// the pointer until the button is let go, and without shift the selection goes.
void Graph::press(VP::Cook &frame, const VP::View &view) {
  if (_typed->focus != _name)
    frame.commands().send(std::format("focus {}", _name));
  for (std::size_t at = _placed.size(); at-- > 0;) {
    if (!VP_VIEW::contains(cut(box(_placed[at]), *_area), _pointer))
      continue;
    const std::string &node = view.nodes[at].name;
    std::vector<std::string> chosen;
    if (_selected_in == _shown)
      chosen = _selection;
    if (_shift || !selected(node)) {
      if (!_shift)
        chosen.clear();
      if (const auto found = std::ranges::find(chosen, node); found != chosen.end())
        chosen.erase(found);
      else
        chosen.push_back(node);
      select(frame.commands(), chosen);
    }
    if (_shift)
      return;
    _grab = Grab{.box = at, .from = _pointer};
    for (std::size_t box = 0; box < _placed.size(); ++box)
      if (selected(view.nodes[box].name))
        _grab->moving.emplace_back(box, _placed[box].offset);
    return;
  }
  if (!_shift && _selected_in == _shown && !_selection.empty())
    select(frame.commands(), {});
  _dragging = true;
}

// Once the pointer moved a few pixels, so a press that wobbles is still a press. The
// first drag places every box where it shows, so a box moved moves no other, and a node
// added later takes its column's place.
void Graph::drag(const VP::View &view, glm::vec2 to) {
  Grab &grab = *_grab;
  if (grab.box >= _placed.size() ||
      (!grab.moved && glm::length(to - grab.from) < drag_least))
    return;
  if (!grab.moved)
    for (std::size_t at = 0; at < _placed.size(); ++at)
      _arranged.places.try_emplace(view.nodes[at].name, _placed[at].offset);
  grab.moved = true;
  const glm::ivec2 by(glm::round((to - grab.from) / _arranged.zoom));
  for (const auto &[box, offset] : grab.moving)
    if (box < _placed.size()) {
      _arranged.places.insert_or_assign(view.nodes[box].name, offset + by);
      _placed[box].offset = offset + by; // so this frame shows it there
    }
}

// A box dragged, or the ground, is saved at once. A box pressed and let go where it was
// stays the one selected, and opens the file of its row pressed.
void Graph::release(VP::Cook &frame, const VP::View *view, glm::uvec2 cell) {
  if (std::exchange(_dragging, false) && _unsaved)
    save(frame);
  if (!_grab)
    return;
  const Grab grab = *std::exchange(_grab, std::nullopt);
  if (grab.moved) {
    save(frame);
    return;
  }
  if (!view || grab.box >= _placed.size())
    return;
  if (const std::string &node = view->nodes[grab.box].name; _selection.size() > 1)
    select(frame.commands(), std::span(&node, 1));
  open(frame, *view, grab.box, cell);
}

// A file's row opens the file, as a line typed would; the name's row opens none.
void Graph::open(VP::Cook &frame, const VP::View &view, std::size_t at, glm::uvec2 cell) {
  const VP::Node &node = view.nodes[at];
  const float row =
      ((_pointer.y - on_screen(glm::vec2(_placed[at].offset)).y) / _arranged.zoom -
       static_cast<float>(pad)) /
      static_cast<float>(cell.y);
  if (node.files.empty() || row < 1.0f)
    return;
  const auto file = std::min(static_cast<std::size_t>(row) - 1, node.files.size() - 1);
  frame.commands().send(std::format(
      "open {}", (folder_of(view, node.name) / node.files[file]).generic_string()));
}

// The wheel zooms, about the pointer, which stays over the same place of the graph; its
// turn sideways does nothing yet.
void Graph::turn(VP::Cook &frame, glm::vec2 turn) {
  _arranged.wheel(turn.y, _pointer - glm::vec2(_area->offset));
  _unsaved = frame.index();
}

bool Graph::selected(std::string_view node) const {
  return _selected_in == _shown &&
         std::ranges::find(_selection, node) != _selection.end();
}

// As a line, so a press selects as a script or an agent does (V06).
void Graph::select(VP::CommandPort &commands, std::span<const std::string> nodes) const {
  std::string line = std::format("{}: select", _shown);
  for (const std::string &node : nodes)
    line.append(" ").append(node);
  commands.send(line);
}

// The nodes selected and every node inside them, deepest first, as node remove takes no
// node that holds one. A connection a node going writes is let go, and one it reads is
// made again without it, so the nodes that stay keep reading. Each is a line of this
// command, so the log keeps them as one group (V08), and their boxes leave the .ini.
// What it reads of the view is copied first, since each line replaces the view.
void Graph::erase(VP::Call &call, const VP::View &view) {
  std::vector<std::string> going;
  for (const VP::Node &node : view.nodes)
    if (std::ranges::any_of(_selection, [&](const std::string &chosen) {
          return node.name == chosen || node.name.starts_with(chosen + '.');
        }))
      going.push_back(node.name);
  if (going.empty())
    throw std::runtime_error(
        "no node is selected; select or a press on a box selects one");
  const auto goes = [&](const VP::Endpoint &end) {
    return !end.view() && std::ranges::find(going, end.node) != going.end();
  };
  const std::vector<VP::Connection> connections = view.connections;
  for (const VP::Connection &connection : connections) {
    if (!goes(connection.from) && std::ranges::none_of(connection.to, goes))
      continue;
    call.commands().send(std::format("disconnect {}", connection.name));
    std::string staying;
    for (const VP::Endpoint &end : connection.to)
      if (!goes(end))
        staying.append(" ").append(end.text());
    if (!goes(connection.from) && !staying.empty())
      call.commands().send(std::format(
          "connect {} {}{}", connection.name, connection.from.text(), staying));
  }
  std::ranges::stable_sort(going, std::greater{}, [](const std::string &name) {
    return std::ranges::count(name, '.');
  });
  for (const std::string &node : going) {
    call.commands().send(std::format("node remove {}", node));
    _arranged.places.erase(node);
  }
  _selection.clear();
  _grab.reset();
  keep(call.files());
}

// A save the disk refuses is named in the log, and the graph goes on.
void Graph::save(VP::Cook &frame) {
  try {
    keep(frame.files());
  } catch (const std::runtime_error &failure) {
    frame.log(VP::Level::error, failure.what());
  }
}

// Whole, through the file port, which writes a temp file and renames it over the old
// one (RA04). Throws naming a save the disk refuses.
void Graph::keep(VP::FilePort &files) {
  _unsaved.reset();
  if (_ini.empty() || _refused)
    return;
  std::string text(kept_header);
  text += std::format("pan  = {} {}\nzoom = {:.3f}\n",
                      std::lround(_arranged.pan.x),
                      std::lround(_arranged.pan.y),
                      _arranged.zoom);
  for (const auto &[node, place] : _arranged.places)
    text += std::format("{}{} = {} {}\n", place_key, node, place.x, place.y);
  files.save(_ini, text);
}

// The ground under the whole area, in the role of its lines, which the graph's shaders
// draw before the parts that draw the rest; then a border and a fill a box. A box
// selected has a wider border, in the accent color.
void Graph::write(VP::Cook &frame, const VP::View &view, glm::uvec2 cell) {
  frame.write(_ground, 1).front() = {
      .offset = _area->offset, .extent = _area->extent, .role = VP_VIEW::role("border")};
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  std::uint32_t written = 0;
  for (std::size_t at = 0; at < _placed.size(); ++at) {
    const bool chosen = selected(view.nodes[at].name);
    const auto inset = glm::ivec2(chosen ? selected_width : border_width);
    const VP_VIEW::Rect outer = box(_placed[at]);
    const VP_VIEW::Rect border =
        cut({.offset = outer.offset,
             .extent = outer.extent,
             .role = chosen ? VP_VIEW::role("accent") : VP_VIEW::role("border")},
            *_area);
    if (border.extent.x == 0)
      continue;
    rects[written++] = border;
    rects[written++] =
        cut({.offset = outer.offset + inset,
             .extent = glm::uvec2(
                 glm::max(glm::ivec2(outer.extent) - 2 * inset, glm::ivec2(0))),
             .role = VP_VIEW::role("panel")},
            *_area);
  }
  frame.write(_rects, written); // the used length, which rects draws
  write_labels(frame, view, cell);
  write_curves(frame, cell);
}

// A box's name a cell in from its left, between its pads, and each file a row below it.
void Graph::write_labels(VP::Cook &frame, const VP::View &view, glm::uvec2 cell) {
  const std::span<VP_VIEW::Label> labels = frame.write(_labels);
  const std::span<VP_VIEW::Character> characters = frame.write(_characters);
  _label_count = 0;
  _character_count = 0;
  for (std::size_t at = 0; at < _placed.size(); ++at) {
    const VP::Node &node = view.nodes[at];
    const glm::vec2 text = glm::vec2(_placed[at].offset) +
                           glm::vec2(static_cast<float>(name_margin * cell.x), pad);
    write_row(
        on_screen(text), node.name, VP_VIEW::role("accent"), cell, labels, characters);
    for (std::size_t row = 0; row < node.files.size(); ++row)
      write_row(on_screen(text + glm::vec2(0.0f, static_cast<float>((row + 1) * cell.y))),
                node.files[row],
                VP_VIEW::role("text"),
                cell,
                labels,
                characters);
  }
  frame.write(_labels, _label_count);
  frame.write(_characters, _character_count);
}

// The characters left of the area are dropped, and its room ends at the area's right,
// which glyphs cuts at; a row the area does not hold whole is not written. Its cells are
// the font's at the graph's zoom.
void Graph::write_row(glm::vec2 at,
                      std::string_view row,
                      std::uint32_t role,
                      glm::uvec2 cell,
                      std::span<VP_VIEW::Label> labels,
                      std::span<VP_VIEW::Character> characters) {
  const VP_VIEW::Rect &area = *_area;
  const glm::vec2 size = glm::vec2(cell) * _arranged.zoom;
  const auto right =
      static_cast<float>(area.offset.x + static_cast<std::int32_t>(area.extent.x));
  if (_label_count == labels.size() || at.y < static_cast<float>(area.offset.y) ||
      at.y + size.y >
          static_cast<float>(area.offset.y) + static_cast<float>(area.extent.y))
    return;
  std::size_t skip = 0;
  if (at.x < static_cast<float>(area.offset.x)) {
    skip = static_cast<std::size_t>(
        std::ceil((static_cast<float>(area.offset.x) - at.x) / size.x));
    at.x += static_cast<float>(skip) * size.x;
  }
  if (skip >= row.size() || at.x >= right)
    return;
  const glm::ivec2 offset(glm::round(at));
  const auto shown = static_cast<std::uint32_t>(
      std::min(row.size() - skip, std::size_t{characters.size() - _character_count}));
  for (std::uint32_t character = 0; character < shown; ++character)
    characters[_character_count + character] = {
        .code = static_cast<unsigned char>(row[skip + character]), .label = _label_count};
  labels[_label_count++] = {
      .offset = offset,
      .extent = {static_cast<std::uint32_t>(right) - static_cast<std::uint32_t>(offset.x),
                 static_cast<std::uint32_t>(std::ceil(size.y))},
      .role = role,
      .first = _character_count,
      .count = shown,
      .scale = _arranged.zoom};
  _character_count += shown;
}

// From the middle of the writer's name row on its right side to the middle of the
// reader's on its left, along each lane it keeps on the way, a curve to the lane and one
// along it.
void Graph::write_curves(VP::Cook &frame, glm::uvec2 cell) {
  const std::span<VP_VIEW::Curve> curves = frame.write(_curves);
  const float middle = static_cast<float>(cell.y + 2 * pad) / 2.0f;
  const auto end = [&](const Placed &placed, bool right) {
    const float across = right ? static_cast<float>(placed.extent.x) : 0.0f;
    return on_screen(glm::vec2(placed.offset) + glm::vec2(across, middle));
  };
  std::uint32_t written = 0;
  std::size_t lane = 0;
  for (std::size_t at = 0; at < _ends.size(); at += 2) {
    glm::vec2 from = end(_placed[_ends[at]], true);
    for (; lane < _lanes.size() && _lanes[lane].pair == at / 2; ++lane) {
      const glm::vec2 left =
          on_screen(glm::vec2(_lanes[lane].offset) + glm::vec2(0.0f, middle));
      const glm::vec2 right =
          left + glm::vec2(static_cast<float>(_lanes[lane].width) * _arranged.zoom, 0.0f);
      write_curve(curves, written, from, left);
      write_curve(curves, written, left, right);
      from = right;
    }
    write_curve(curves, written, from, end(_placed[_ends[at + 1]], false));
  }
  frame.write(_curves, written);
}

// Whole, even with an end out of view: the curves part cuts it at the area's edge, which
// the ground hands it, so a curve to a box dragged out runs to the edge.
void Graph::write_curve(std::span<VP_VIEW::Curve> curves,
                        std::uint32_t &written,
                        glm::vec2 from,
                        glm::vec2 to) const {
  if (written < curves.size())
    curves[written++] = {.from = from, .to = to, .role = VP_VIEW::role("accent")};
}

glm::vec2 Graph::on_screen(glm::vec2 at) const {
  return glm::vec2(_area->offset) + _arranged.on_screen(at);
}

VP_VIEW::Rect Graph::box(const Placed &placed) const {
  const glm::vec2 from = glm::round(on_screen(glm::vec2(placed.offset)));
  const glm::vec2 to =
      glm::round(on_screen(glm::vec2(placed.offset + glm::ivec2(placed.extent))));
  return {.offset = glm::ivec2(from), .extent = glm::uvec2(to - from)};
}

VP_OPERATORS(registry) {
  registry.add<Graph>("Graph");
}
