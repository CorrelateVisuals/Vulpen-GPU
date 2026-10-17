// The graph part's operator. It runs past 800 lines (CPP12), as a class in the unnamed
// namespace, so that two copies of the part never clash, keeps its methods to one file;
// what needs none of its state lives in the headers beside it.
#include "Graph.h"

#include "runtime/View.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

// A box is its border, then its fill, and one holding boxes is that over its rows, then
// its sides and its bottom round the boxes inside it, which show through; then a mark a
// port, where its curves land.
constexpr std::uint32_t box_rects = 5;
constexpr std::uint32_t mark_room = 8; // marks a box, on average
constexpr std::uint32_t select_rects = 4; // the select box's edges
constexpr std::uint32_t rect_room = (box_rects + mark_room) * node_room + select_rects;
constexpr std::uint32_t label_room = 4096;      // the names, rows and ports together
constexpr std::uint32_t character_room = 32768; // their characters
// A curve a line and a corner of each reader's way, as many as its levels have.
constexpr std::uint32_t curve_room = 8192;
constexpr std::int32_t border_width = 1;        // in pixels
constexpr std::int32_t selected_width = 2;      // a selected box's border, in pixels
constexpr float mark_size = 6.0f;               // a port's mark, in pixels at zoom 1
constexpr float least_reach = 32.0f;            // a bend's handles, at least, in pixels
// A cubic's handles, of its radius, that keep it on a quarter circle.
constexpr float quarter = 0.5523f;
// The role each layer's curves are drawn in, by VP_VIEW::Layer: a connection in the
// accent color, and each kind of relation in a hue the editor shows code in, so the
// theme keeps every color.
constexpr std::array<std::uint32_t, VP_VIEW::layer_names.size()> layer_roles{
    VP_VIEW::role("accent"),
    VP_VIEW::role("directive"),
    VP_VIEW::role("keyword"),
    VP_VIEW::role("number"),
    VP_VIEW::role("string")};
constexpr float drag_least = 3.0f;              // pixels a press moves before a box follows
// Zoomed in to this many times as near as home is, the graph shows the inside of the box
// under the pointer, as the POC did; zoomed out past this share of it, a level up.
constexpr float enter_times = 3.0f;
constexpr float leave_share = 0.5f;
constexpr std::string_view title = "graph";

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

// Whether two Rects share a pixel.
bool touches(const VP_VIEW::Rect &rect, const VP_VIEW::Rect &by) {
  return cut(rect, by).extent.x != 0;
}

bool inside(const VP_VIEW::Rect &area, glm::vec2 point) {
  return VP_VIEW::contains(area, point);
}

// The Rect from one corner to the other, rounded to whole pixels.
VP_VIEW::Rect between(glm::vec2 from, glm::vec2 to, std::uint32_t role) {
  const glm::ivec2 low(glm::round(from));
  const glm::ivec2 high(glm::round(to));
  return {.offset = low, .extent = glm::uvec2(glm::max(high - low, glm::ivec2(0))), .role = role};
}

} // namespace

void Graph::bind(VP::Bind &node) {
  _area = &node.input<VP_VIEW::Rect>("area");
  _font = &node.input<VP_VIEW::Font>("font");
  _typed = &node.input<VP_VIEW::Typed>("typed");
  _name = node.name();
  _view = node.view_name();
  _inside = node.sets("inside") ? node.param<std::string>("inside") : std::string();
  _relations = &node.input<VP_VIEW::Relations>("relations");
  _layers = node.sets("relations") ? layers_of(node.param<std::string>("relations"))
                                   : every_layer;
  _buttons = &node.output<VP_VIEW::Items>("layers");
  buttons(*_buttons, _layers, _view, _name);
  _select = node.command("select [<node>...]",
                         "selects nodes of the view the graph shows, or views it hosts, "
                         "in place of those selected; with none, none");
  _selected = node.command(
      "selected", "lists the nodes and views the graph has selected, a line each");
  _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
  _labels = node.upload<VP_VIEW::Label>("labels", label_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  _curves = node.upload<VP_VIEW::Curve>("curves", curve_room);
  _tabs = &node.output<VP_VIEW::Items>("tabs");
  _ground = node.upload<VP_VIEW::Rect>("ground");
  _pan = node.value<glm::vec2>("pan");
  _zoom = node.value<float>("zoom");
}

// The current view, where the terminals' lines go, the one vulpen started with too, or the
// folder the relations part reads in its place, until a view is switched to. The keys
// once it is known, and before it is read for the frame, as a removal changes it.
void Graph::cook(VP::Cook &frame) {
  const std::string shown(frame.commands().current());
  const bool folder = !_relations->folder.empty();
  if (_tabs->empty() || shown != _shown) {
    if (!_tabs->empty() && folder)
      show_view(frame);
    show(frame, frame.view(shown), shown);
  }
  for (const VP::Event &event : _typed->to(_name))
    if (event.kind == VP::Event::Kind::key && event.down)
      key(frame, event.name);
  const VP::View *const edited = folder ? &_relations->tree : frame.view(shown);
  const VP::View *const flat = folder ? &_relations->tree : frame.unfolded(shown);
  if (edited != _built_edited || flat != _built_flat || _inside != _built_inside ||
      _relations->folder != _built_folder || _relations->changes != _built_changes ||
      _layers != _built_layers)
    build(edited, flat);
  const glm::uvec2 cell = _font->cell;
  const bool room = edited && flat && _area->extent.x != 0 && _area->extent.y != 0 &&
                    cell.x != 0 && cell.y != 0;
  const auto lay_out = [&] {
    if (room && (!_laid || cell != _laid_cell)) {
      _layout.lay_out(_boxes, _arranged, cell);
      _laid = true;
      _laid_cell = cell;
    }
  };
  lay_out();
  if (room && std::exchange(_homing, false)) {
    home(cell);
    _home_zoom = _pan_zoom.zoom;
  }
  // A press may send a line that edits the view shown, as open does the ide's own, or
  // closes it; the boxes, built from the view before, stay until the next frame.
  for (const VP::Event &event : frame.input().events())
    follow(frame, room, event, cell);
  lay_out(); // as a drag placed a box
  frame.set(_pan, _pan_zoom.pan);
  frame.set(_zoom, _pan_zoom.zoom);
  if (room) {
    write(frame, cell);
    return;
  }
  // No room, no work: nothing written draws nothing.
  frame.write(_ground, 0);
  frame.write(_rects, 0);
  frame.write(_labels, 0);
  frame.write(_characters, 0);
  frame.write(_curves, 0);
}

// On the view the graph shows, the current one, as the terminals' lines go to it, so a
// line to any other is refused rather than acting where nobody looks.
void Graph::command(VP::Call &call) {
  const std::string shown(call.commands().current());
  if (call.view(shown) != &call.view())
    throw std::runtime_error(std::format(
        "the graph shows {}, so its lines go there, as {}: select",
        shown.empty() ? std::string_view("the view vulpen started with") : shown,
        shown));
  if (_selected_in != shown) // a selection in a view hosted before
    _selection.clear();
  const VP::View &view = call.view();
  const auto boxed = [&](std::string_view name) {
    const auto &children = view.children;
    return std::ranges::find(view.nodes, name, &VP::Node::name) != view.nodes.end() ||
           std::ranges::find(children, name, &VP::Child::name) != children.end();
  };
  if (call.is(_select)) {
    std::vector<std::string> chosen;
    for (const std::string_view name : call.arguments()) {
      if (!boxed(name))
        throw std::runtime_error(
            std::format("no node or view it hosts is named {}; ls lists them", name));
      if (std::ranges::find(chosen, name) == chosen.end())
        chosen.emplace_back(name);
    }
    for (const std::string &name : chosen)
      raise(name);
    _selection = std::move(chosen);
    _selected_in = shown;
  } else if (call.is(_selected)) {
    for (const std::string &name : _selection)
      if (boxed(name))
        call.reply(name);
  }
}

// delete or backspace removes what is selected; escape lets go of it, and with none of
// the focus, as the editor and the terminal do. The home key brings the graph home once
// the frame has laid the boxes out.
void Graph::key(VP::Cook &frame, std::string_view key) {
  const bool chosen = _selected_in == _shown && !_selection.empty();
  const VP::View *const view = frame.view(_shown);
  if ((key == "delete" || key == "backspace") && chosen && view) {
    erase(frame, *view);
  } else if (key == "escape" && chosen) {
    frame.commands().send(std::format("{}: select", _shown));
  } else if (key == "escape") {
    frame.commands().send("focus");
  } else if (key == PanZoom::home_key) {
    _homing = true;
  }
}

// The tab names the view, and the boxes its .ini kept show where they were left, from
// home. An .ini that holds a mistake is named in the log and kept as it is, so no save
// writes over what a person typed; the graph then lays the view out from the graph alone.
// A selection in another view goes, and the boxes are built anew, as another view may
// have taken the place in memory of the one they were built from.
void Graph::show(VP::Cook &frame, const VP::View *view, std::string_view name) {
  _shown = name;
  if (_selected_in != name)
    _selection.clear();
  _raised.clear();
  name_tab();
  _arranged = {};
  _ini.clear();
  _refused = false;
  _homing = true;
  _grab.reset();
  _select_from.reset();
  _built_edited = nullptr;
  _built_flat = nullptr;
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

// A box pressed may be one the boxes no longer hold. A level shown anew starts at home. A
// folder always shows an inside: the one the inside param names, or the folder's own;
// the paths past it are named from the folder holding it, and a view's from its folder.
void Graph::build(const VP::View *edited, const VP::View *flat) {
  const std::string &folder = _relations->folder;
  if (_inside != _built_inside || folder != _built_folder)
    _homing = true;
  _built_edited = edited;
  _built_flat = flat;
  _built_inside = _inside;
  _built_folder = folder;
  _built_changes = _relations->changes;
  _built_layers = _layers;
  _boxes = {};
  if (edited && flat && (folder.empty() || !flat->nodes.empty())) {
    const bool inside = std::ranges::find(flat->nodes, _inside, &VP::Node::name) !=
                        flat->nodes.end();
    _boxes = boxes_of(*edited,
                      *flat,
                      folder.empty() || inside ? _inside : flat->nodes.front().name,
                      _relations->relations,
                      folder.empty() ? edited->file.parent_path()
                                     : std::filesystem::path(folder).parent_path(),
                      !folder.empty(),
                      _layers);
  }
  draw_order(_boxes, _raised, _order);
  name_tab();
  _laid = false;
  _grab.reset();
}

// The view again, in place of the folder the relations part reads.
void Graph::show_view(VP::Cook &frame) const {
  frame.commands().send(
      std::format("{}: param unset {} folder", _relations->view, _relations->node));
}

// The tab names the view shown and the node whose inside shows, or the folder shown.
void Graph::name_tab() {
  std::string label(title);
  if (!_shown.empty() && _relations->folder.empty())
    label += std::format(" - {}", _shown);
  if (_boxes.frame != no_box)
    label += std::format(" - {}", _boxes.boxes[_boxes.frame].name);
  _tabs->assign(1, {.label = std::move(label), .current = true});
}

// Unmoved at zoom 1 while every box shows whole there, as the graph first shows them;
// else the boxes from their top left, as far in as the graph puts its first box, and as
// near as lets them all show, never nearer than zoom 1, so text keeps its size where it
// can. Where even the least zoom cannot hold them, from their top left at the least.
void Graph::home(glm::uvec2 cell) {
  _pan_zoom.home();
  const std::span<const Placed> placed = _layout.placed();
  if (placed.empty())
    return;
  glm::ivec2 low = placed.front().at;
  glm::ivec2 high = low;
  for (const Placed &box : placed) {
    low = glm::min(low, box.at);
    high = glm::max(high, box.at + glm::ivec2(box.extent));
  }
  const glm::ivec2 room(_area->extent);
  if (low.x >= 0 && low.y >= 0 && high.x <= room.x && high.y <= room.y)
    return;
  const glm::vec2 margin(margin_of(cell));
  const glm::vec2 fits = (glm::vec2(room) - 2.0f * margin) / glm::vec2(high - low);
  _pan_zoom.zoom = std::clamp(std::min(fits.x, fits.y), PanZoom::least, 1.0f);
  _pan_zoom.pan = margin - glm::vec2(low) * _pan_zoom.zoom;
}

// In the order the events came, so a press is tested where the pointer was then. With
// no view in its area it takes no press and no wheel, only where the pointer went.
void Graph::follow(VP::Cook &frame, bool room, const VP::Event &event, glm::uvec2 cell) {
  if (event.kind == VP::Event::Kind::pointer) {
    if (_grab && room) {
      drag(event.at);
    } else if (_dragging) {
      _pan_zoom.pan += event.at - _pointer;
    }
    _pointer = event.at;
  } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
    if (!event.down)
      release(frame, room, cell);
    else if (room && inside(*_area, _pointer))
      press(frame, event.presses);
  } else if (event.kind == VP::Event::Kind::wheel && room && inside(*_area, _pointer)) {
    turn(frame, event.turn);
  } else if (event.kind == VP::Event::Kind::key && event.name == "shift") {
    _shift = event.down;
  } else if (event.kind == VP::Event::Kind::focus && !event.down) {
    // Let go while the window had no focus, which no event says.
    _shift = false;
  }
}

// It takes the focus. On a box, the box drawn last over the others: a second press goes
// into its inside, or out of the frame's. A box a line selects: shift adds it to the
// selection or takes it out, or it is selected, unless it is already, and grabbed with
// every box selected, so a drag moves them all. Any other box: the selection goes, and it
// is raised and grabbed alone, or, where a drag cannot move it, as a folder's, grabbed
// so letting go opens its row. On the ground, the frame's inside too, with shift, a
// select box from there to the pointer until the button is let go; without, the
// selection goes, and the graph follows the pointer until then.
void Graph::press(VP::Cook &frame, std::uint32_t presses) {
  if (_typed->focus != _name)
    frame.commands().send(std::format("focus {}", _name));
  const bool chosen_here = _selected_in == _shown && !_selection.empty();
  if (const std::size_t at = hit(_pointer); at != no_box) {
    if (presses == 2 && enter(frame, at))
      return;
    const Box &pressed = _boxes.boxes[at];
    if (!pressed.own) {
      if (_shift)
        return;
      if (chosen_here)
        select(frame.commands(), {});
      _grab = Grab{.box = at, .from = _pointer};
      if (pressed.kept) {
        raise(pressed.name);
        _grab->moving.emplace_back(at, _layout.placed()[at].offset);
      }
      return;
    }
    std::vector<std::string> chosen;
    if (_selected_in == _shown)
      chosen = _selection;
    if (_shift || !selected(pressed.name)) {
      if (!_shift)
        chosen.clear();
      if (const auto found = std::ranges::find(chosen, pressed.name);
          found != chosen.end())
        chosen.erase(found);
      else
        chosen.push_back(pressed.name);
      select(frame.commands(), chosen);
    }
    if (_shift)
      return;
    _grab = Grab{.box = at, .from = _pointer};
    for (std::size_t box = 0; box < _boxes.boxes.size(); ++box)
      if (_boxes.boxes[box].own && selected(_boxes.boxes[box].name))
        _grab->moving.emplace_back(box, _layout.placed()[box].offset);
    return;
  }
  if (_shift) {
    _select_from = _pointer;
    return;
  }
  if (chosen_here)
    select(frame.commands(), {});
  _dragging = true;
}

// Once the pointer moved a few pixels, so a press that wobbles is still a press. The
// first drag places every box of the view where it shows, so a box moved moves no other,
// and a node added later takes its column's place. A box inside another stays inside it,
// which grows to hold it, and moves with it, so a box selected inside one selected moves
// once. A box a drag cannot move moves the graph, as the ground does.
void Graph::drag(glm::vec2 to) {
  Grab &grab = *_grab;
  const std::span<const Placed> placed = _layout.placed();
  if (grab.box >= placed.size() ||
      (!grab.moved && glm::length(to - grab.from) < drag_least))
    return;
  if (grab.moving.empty()) {
    _pan_zoom.pan += to - (grab.moved ? _pointer : grab.from);
    grab.moved = true;
    return;
  }
  if (!grab.moved)
    for (std::size_t at = 0; at < placed.size(); ++at)
      if (_boxes.boxes[at].kept)
        _arranged.places.try_emplace(_boxes.boxes[at].name, placed[at].offset);
  grab.moved = true;
  const glm::ivec2 by(glm::round((to - grab.from) / _pan_zoom.zoom));
  const auto moving = [&](std::size_t box) {
    return std::ranges::find(grab.moving, box, &std::pair<std::size_t, glm::ivec2>::first) !=
           grab.moving.end();
  };
  for (const auto &[box, offset] : grab.moving) {
    std::size_t up = _boxes.boxes[box].parent;
    while (up != no_box && !moving(up))
      up = _boxes.boxes[up].parent;
    if (up != no_box)
      continue;
    const bool nested = _boxes.boxes[box].parent != no_box;
    _arranged.places.insert_or_assign(_boxes.boxes[box].name,
                                      nested ? glm::max(offset + by, glm::ivec2(0))
                                             : offset + by);
  }
  _laid = false; // so this frame shows them there
}

// A box dragged is saved at once. A select box selects what it touches. A box pressed
// and let go where it was stays the one selected, and opens the file of its row pressed.
void Graph::release(VP::Cook &frame, bool room, glm::uvec2 cell) {
  _dragging = false;
  if (const std::optional<glm::vec2> from = std::exchange(_select_from, std::nullopt);
      from && room)
    select_touched(frame.commands(), *from);
  if (!_grab)
    return;
  const Grab grab = *std::exchange(_grab, std::nullopt);
  if (grab.moved) {
    if (!grab.moving.empty())
      save(frame);
    return;
  }
  if (!room || grab.box >= _boxes.boxes.size())
    return;
  if (const Box &pressed = _boxes.boxes[grab.box]; pressed.own && _selection.size() > 1)
    select(frame.commands(), std::span(&pressed.name, 1));
  open(frame, grab.box, cell);
}

// A row that names a file opens it, as a line typed would: a node's file, from its folder
// in the view or the library, the manifest of the recipe a node uses, or that of a view
// where it is hosted from; a box's name and its ports open none.
void Graph::open(VP::Cook &frame, std::size_t at, glm::uvec2 cell) {
  const float row =
      ((_pointer.y - on_screen(glm::vec2(_layout.placed()[at].at)).y) / _pan_zoom.zoom -
       static_cast<float>(pad)) /
      static_cast<float>(cell.y);
  const std::span<const BoxRow> rows = rows_in(_boxes, at);
  if (row < 1.0f && !_boxes.boxes[at].opens.empty())
    frame.commands().send(
        std::format("open {}", _boxes.boxes[at].opens.generic_string()));
  if (row < 1.0f || static_cast<std::size_t>(row) > rows.size())
    return;
  if (const BoxRow &pressed = rows[static_cast<std::size_t>(row) - 1];
      !pressed.opens.empty())
    frame.commands().send(std::format("open {}", pressed.opens.generic_string()));
}

// The wheel zooms, about the pointer, which stays over the same place of the graph; its
// turn sideways does nothing yet. Zoomed in to three times as near as home, or as near
// as it goes, the graph shows the inside of the box under the pointer; zoomed out past
// half as near as home, or as far as it goes, a level up.
void Graph::turn(VP::Cook &frame, glm::vec2 turn) {
  _pan_zoom.wheel(turn.y, _pointer - glm::vec2(_area->offset));
  if (turn.y < 0.0f && (_pan_zoom.zoom < _home_zoom * leave_share ||
                        _pan_zoom.zoom <= PanZoom::least)) {
    leave(frame);
    return;
  }
  if (turn.y <= 0.0f ||
      _pan_zoom.zoom < std::min(_home_zoom * enter_times, PanZoom::most))
    return;
  if (const std::size_t at = hit(_pointer); at != no_box && at != _boxes.frame)
    enter(frame, at);
}

// A box's inside, as a second press or the wheel goes into it: a view's, as view switch,
// or a node's holding nodes, as the inside param, which a line sets as a press does
// (V06); and out of the frame's, a level up. False for a box with no inside.
bool Graph::enter(VP::Cook &frame, std::size_t at) {
  const Box &entered = _boxes.boxes[at];
  if (entered.kind == Kind::view)
    frame.commands().send(std::format("view switch {}", entered.name));
  else if (at == _boxes.frame)
    leave(frame);
  else if (entered.holds)
    frame.commands().send(
        std::format("{}: param set {} inside {}", _view, _name, entered.name));
  return entered.kind == Kind::view || entered.holds;
}

// Out of the node whose inside shows, to the inside of the node holding it, or the top;
// out of a folder to the one holding it, or from the folder opened, to the view again.
void Graph::leave(VP::Cook &frame) {
  if (_boxes.frame == no_box || !_built_flat)
    return;
  const std::string &left = _boxes.boxes[_boxes.frame].name;
  if (!_relations->folder.empty()) {
    if (const std::size_t slash = left.rfind('/'); slash == std::string::npos)
      show_view(frame);
    else
      frame.commands().send(
          std::format("{}: param set {} inside {}", _view, _name, left.substr(0, slash)));
    return;
  }
  const std::size_t holder = parent_of(_built_flat->nodes, left);
  frame.commands().send(
      holder == no_box ? std::format("{}: param unset {} inside", _view, _name)
                       : std::format("{}: param set {} inside {}",
                                     _view,
                                     _name,
                                     _built_flat->nodes[holder].name));
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

// Each box the select box touches joins the selection, as shift with a press adds one,
// but those no line selects, as a node's of a recipe; the select box is cut at the area,
// as it shows, so only what shows of a box is touched, and of a frame its rows.
void Graph::select_touched(VP::CommandPort &commands, glm::vec2 from) const {
  std::vector<std::string> chosen;
  if (_selected_in == _shown)
    chosen = _selection;
  const std::size_t before = chosen.size();
  const VP_VIEW::Rect swept = select_box(from);
  for (std::size_t at = 0; at < _layout.placed().size(); ++at)
    if (const Box &touched = _boxes.boxes[at];
        touched.own && touches(solid(at), swept) && !selected(touched.name))
      chosen.push_back(touched.name);
  if (chosen.size() != before)
    select(commands, chosen);
}

// The nodes and the views selected go, a line each, as node remove takes a node with the
// nodes inside it and the part it has in connections, and view close stops hosting a
// view; a node inside another selected goes with that one. Their boxes leave the .ini.
// What it reads of the view is read first, since each line replaces the view.
void Graph::erase(VP::Cook &frame, const VP::View &view) {
  std::vector<std::string> lines;
  for (const std::string &name : _selection) {
    if (std::ranges::any_of(_selection, [&](const std::string &other) {
          return name.starts_with(other + '.');
        }))
      continue;
    const bool hosted =
        std::ranges::find(view.children, name, &VP::Child::name) != view.children.end();
    lines.push_back(
        std::format("{}: {} {}", _shown, hosted ? "view close" : "node remove", name));
    std::erase_if(_arranged.places, [&](const auto &place) {
      return place.first == name || place.first.starts_with(name + '.');
    });
  }
  for (const std::string &line : lines)
    frame.commands().send(line);
  _selection.clear();
  _grab.reset();
  keep(frame.files());
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
  if (_ini.empty() || _refused)
    return;
  files.save(_ini, text_of(_arranged));
}

// The ground under the whole area, in the role of its lines, which the graph's shaders
// draw before the parts that draw the rest; then each box in the order drawn, a box
// inside another after it, so it shows over it.
void Graph::write(VP::Cook &frame, glm::uvec2 cell) {
  frame.write(_ground, 1).front() = {
      .offset = _area->offset, .extent = _area->extent, .role = VP_VIEW::role("border")};
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  std::uint32_t written = 0;
  for (const std::size_t at : _order)
    write_box(rects, written, at);
  if (_select_from)
    write_select_box(rects, written);
  frame.write(_rects, written); // the used length, which rects draws
  write_labels(frame, cell);
  write_curves(frame);
}

// A border and a fill, the border in its kind's color where the .ini gives one, wider
// and in the accent color while it is selected; one holding boxes is filled over its
// rows alone, and framed below them, so the boxes inside it and the curves between them
// show. Each is cut at the area.
void Graph::write_box(std::span<VP_VIEW::Rect> rects,
                      std::uint32_t &written,
                      std::size_t at) const {
  const Box &shown = _boxes.boxes[at];
  const bool chosen = shown.own && selected(shown.name);
  const float width = static_cast<float>(chosen ? selected_width : border_width);
  const std::uint32_t edge =
      chosen ? VP_VIEW::role("accent") : color_of(at).value_or(VP_VIEW::role("border"));
  const Placed &placed = _layout.placed()[at];
  const bool holds = placed.inside < static_cast<std::int32_t>(placed.extent.y);
  const glm::vec2 from = on_screen(glm::vec2(placed.at));
  const glm::vec2 to = on_screen(glm::vec2(placed.at + glm::ivec2(placed.extent)));
  const float below = holds ? on_screen(glm::vec2(placed.at) +
                                        glm::vec2(0.0f, static_cast<float>(placed.inside)))
                                  .y
                            : to.y;
  const auto add = [&](const VP_VIEW::Rect &rect) {
    if (const VP_VIEW::Rect shows = cut(rect, *_area);
        shows.extent.x != 0 && written < rects.size())
      rects[written++] = shows;
  };
  add(between(from, {to.x, below}, edge));
  add(between(from + width, {to.x - width, holds ? below : to.y - width},
              VP_VIEW::role("panel")));
  if (holds) {
    add(between({from.x, below}, {from.x + width, to.y}, edge));
    add(between({to.x - width, below}, to, edge));
    add(between({from.x, to.y - width}, to, edge));
  }
  const std::span<const BoxRow> rows = rows_in(_boxes, at);
  const float half = mark_size * _pan_zoom.zoom / 2.0f;
  for (std::size_t row = 0; row < shown.ports; ++row)
    for (const bool right : {false, true})
      if (!(right ? rows[row].written : rows[row].text).empty()) {
        const glm::vec2 middle = port_of(at, row, right);
        add(between(middle - half, middle + half, VP_VIEW::role("accent")));
      }
}

// Its four edges, as wide as a box's border and in the accent color, over the boxes.
void Graph::write_select_box(std::span<VP_VIEW::Rect> rects,
                             std::uint32_t &written) const {
  const VP_VIEW::Rect swept = select_box(*_select_from);
  if (swept.extent.x == 0 || swept.extent.y == 0 || written + select_rects > rects.size())
    return;
  const glm::ivec2 from = swept.offset;
  const glm::ivec2 last = from + glm::ivec2(swept.extent) - border_width;
  const auto width = static_cast<std::uint32_t>(border_width);
  const glm::uvec2 across{swept.extent.x, width};
  const glm::uvec2 down{width, swept.extent.y};
  constexpr std::uint32_t accent = VP_VIEW::role("accent");
  rects[written++] = {.offset = from, .extent = across, .role = accent};
  rects[written++] = {.offset = {from.x, last.y}, .extent = across, .role = accent};
  rects[written++] = {.offset = from, .extent = down, .role = accent};
  rects[written++] = {.offset = {last.x, from.y}, .extent = down, .role = accent};
}

// From where the shift press began to the pointer, either way, cut at the area.
VP_VIEW::Rect Graph::select_box(glm::vec2 from) const {
  const glm::ivec2 low(glm::round(glm::min(from, _pointer)));
  const glm::ivec2 high(glm::round(glm::max(from, _pointer)));
  return cut({.offset = low, .extent = glm::uvec2(high - low)}, *_area);
}

// A box's name a cell in from its left, between its pads, in its kind's color where the
// .ini gives one, and each row a row below it, in its role: a port read a cell in from the
// left edge, and one written as far in from the right. Each box in the order drawn, its
// rows cut at the boxes drawn after it that it lies under.
void Graph::write_labels(VP::Cook &frame, glm::uvec2 cell) {
  Labels written{frame.write(_labels), frame.write(_characters)};
  const auto margin = static_cast<float>(name_margin * cell.x);
  _covers.clear();
  for (const std::size_t at : _order)
    _covers.push_back(cut(solid(at), *_area));
  for (std::size_t drawn = 0; drawn < _order.size(); ++drawn) {
    const std::size_t at = _order[drawn];
    const VP_VIEW::Rect shown = cut(box(at), *_area);
    _over.clear();
    for (const VP_VIEW::Rect &cover : std::span(_covers).subspan(drawn + 1))
      if (touches(cover, shown))
        _over.push_back(cover);
    const Placed &placed = _layout.placed()[at];
    const glm::vec2 text = glm::vec2(placed.at) + glm::vec2(margin, pad);
    write_row(on_screen(text),
              _boxes.boxes[at].title,
              color_of(at).value_or(VP_VIEW::role("accent")),
              cell,
              _over,
              written);
    const std::span<const BoxRow> rows = rows_in(_boxes, at);
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const glm::vec2 left =
          text + glm::vec2(0.0f, static_cast<float>((row + 1) * cell.y));
      if (!rows[row].text.empty())
        write_row(on_screen(left), rows[row].text, rows[row].role, cell, _over, written);
      if (const std::string &port = rows[row].written; !port.empty())
        write_row(on_screen({static_cast<float>(placed.at.x) +
                                 static_cast<float>(placed.extent.x) - margin -
                                 static_cast<float>(port.size() * cell.x),
                             left.y}),
                  port,
                  rows[row].role,
                  cell,
                  _over,
                  written);
    }
  }
  frame.write(_labels, written.label_count);
  frame.write(_characters, written.character_count);
}

// What of the row shows, a Label a piece: inside the area, and not under a box over it,
// which glyphs cuts its characters at, so a row half out of view, or half under a box,
// shows the rest; each piece holds only the characters it shows. Its cells are the
// font's at the graph's zoom.
void Graph::write_row(glm::vec2 at,
                      std::string_view row,
                      std::uint32_t role,
                      glm::uvec2 cell,
                      std::span<const VP_VIEW::Rect> over,
                      Labels &written) const {
  const glm::vec2 size = glm::vec2(cell) * _pan_zoom.zoom;
  const VP_VIEW::Rect whole = cut(
      between(at, at + glm::vec2(static_cast<float>(row.size()) * size.x, size.y), role),
      *_area);
  if (whole.extent.x == 0 || whole.extent.y == 0)
    return;
  const Pieces shown = uncovered(whole, over);
  for (const VP_VIEW::Rect &piece : std::span(shown.rects).first(shown.count)) {
    const glm::vec2 low(piece.offset);
    const glm::vec2 high = low + glm::vec2(piece.extent);
    const auto first =
        static_cast<std::size_t>(std::max(std::floor((low.x - at.x) / size.x), 0.0f));
    const std::size_t last = std::min(
        static_cast<std::size_t>(std::max(std::ceil((high.x - at.x) / size.x), 0.0f)),
        row.size());
    if (first >= last)
      continue;
    const glm::ivec2 offset(
        glm::round(at + glm::vec2(static_cast<float>(first) * size.x, 0.0f)));
    written.add({.offset = offset,
                 .extent = glm::uvec2(glm::max(glm::ivec2(high) - offset, glm::ivec2(0))),
                 .role = role,
                 .scale = _pan_zoom.zoom,
                 .cut = glm::uvec2(glm::max(glm::ivec2(low) - offset, glm::ivec2(0)))},
                row.substr(first, last - first));
  }
}

// Each curve along its waypoints: straight from one to the next, round each corner on a
// quarter circle as wide as half a lane, so two lanes side by side turn apart, or less
// where a line is too short for it; and where it bends, from one to the next with its
// handles reaching half the way across, and at least least_reach, so one running back to
// the left still bends round. A curve goes whole, even with an end out of view: the
// curves part cuts it at the area's edge, which the ground hands it, so a curve to a box
// dragged out runs to the edge.
void Graph::write_curves(VP::Cook &frame) {
  const std::span<VP_VIEW::Curve> curves = frame.write(_curves);
  std::uint32_t written = 0;
  std::uint32_t role = 0;
  const auto add = [&](glm::vec2 from, glm::vec2 to, glm::vec2 leave, glm::vec2 arrive) {
    if (written < curves.size())
      curves[written++] = {
          .from = from, .to = to, .leave = leave, .arrive = arrive, .role = role};
  };
  const std::span<const Waypoint> waypoints = _layout.waypoints();
  const std::span<const Route> routes = _layout.routes();
  for (std::size_t end = 0; end < routes.size(); ++end) {
    const Route &route = routes[end];
    role = layer_roles[static_cast<std::size_t>(_boxes.ends[end].layer)];
    const std::span<const Waypoint> way = waypoints.subspan(route.first, route.count);
    for (std::size_t at = 0; at + 1 < way.size(); ++at) {
      const glm::vec2 from = on_screen(way[at].at);
      const glm::vec2 to = on_screen(way[at + 1].at);
      if (way[at].bends) {
        const glm::vec2 reach{std::max(std::abs(to.x - from.x) / 2.0f, least_reach),
                              0.0f};
        add(from, to, reach, reach);
        continue;
      }
      const glm::vec2 along = glm::normalize(to - from);
      const glm::vec2 start = from + along * corner(way, at);
      const float round = corner(way, at + 1);
      const glm::vec2 end = to - along * round;
      if (end != start)
        add(start, end, (end - start) / 3.0f, (end - start) / 3.0f);
      if (round > 0.0f) {
        const glm::vec2 next = glm::normalize(on_screen(way[at + 2].at) - to);
        add(end, to + next * round, along * round * quarter, next * round * quarter);
      }
    }
  }
  frame.write(_curves, written);
}

// The radius of the corner a curve turns at a waypoint, on screen: none at its ends,
// where it bends, or where it goes on straight.
float Graph::corner(std::span<const Waypoint> way, std::size_t at) const {
  if (at == 0 || at + 1 >= way.size() || way[at - 1].bends || way[at].bends)
    return 0.0f;
  const glm::vec2 in = way[at].at - way[at - 1].at;
  const glm::vec2 out = way[at + 1].at - way[at].at;
  if (in.x * out.y - in.y * out.x == 0.0f)
    return 0.0f;
  const float half_lane = static_cast<float>(_laid_cell.x) / 2.0f;
  return std::min({half_lane, glm::length(in) / 2.0f, glm::length(out) / 2.0f}) *
         _pan_zoom.zoom;
}

// Of a frame, only its rows hide what lies under them, so a press on its inside is one on
// the ground.
std::size_t Graph::hit(glm::vec2 at) const {
  for (std::size_t drawn = _order.size(); drawn-- > 0;)
    if (const std::size_t box = _order[drawn];
        box < _layout.placed().size() && VP_VIEW::contains(cut(solid(box), *_area), at))
      return box;
  return no_box;
}

// Last in the order raised, so it is drawn last among the boxes beside it, and so is each
// box holding it, so it shows over every box.
void Graph::raise(std::string_view name) {
  if (const auto found = std::ranges::find(_boxes.boxes, name, &Box::name);
      found != _boxes.boxes.end() && found->parent != no_box)
    raise(_boxes.boxes[found->parent].name);
  std::erase(_raised, name);
  _raised.emplace_back(name);
  draw_order(_boxes, _raised, _order);
}

std::optional<std::uint32_t> Graph::color_of(std::size_t box) const {
  return _arranged.colors[static_cast<std::size_t>(_boxes.boxes[box].kind)];
}

glm::vec2 Graph::port_of(std::size_t box, std::size_t row, bool right) const {
  const Placed &placed = _layout.placed()[box];
  const float across = right ? static_cast<float>(placed.extent.x) : 0.0f;
  const float down = static_cast<float>(pad) +
                     (static_cast<float>(row) + 1.5f) * static_cast<float>(_laid_cell.y);
  return on_screen(glm::vec2(placed.at) + glm::vec2(across, down));
}

glm::vec2 Graph::on_screen(glm::vec2 at) const {
  return glm::vec2(_area->offset) + _pan_zoom.on_screen(at);
}

VP_VIEW::Rect Graph::box(std::size_t at) const {
  const Placed &placed = _layout.placed()[at];
  return between(on_screen(glm::vec2(placed.at)),
                 on_screen(glm::vec2(placed.at + glm::ivec2(placed.extent))),
                 0);
}

VP_VIEW::Rect Graph::solid(std::size_t at) const {
  const Placed &placed = _layout.placed()[at];
  const auto rows = std::min(placed.inside, static_cast<std::int32_t>(placed.extent.y));
  return between(on_screen(glm::vec2(placed.at)),
                 on_screen(glm::vec2(placed.at) +
                           glm::vec2(static_cast<float>(placed.extent.x),
                                     static_cast<float>(rows))),
                 0);
}

VP_OPERATORS(registry) {
  registry.add<Graph>("Graph");
}
