// The graph part's operator. It runs past 800 lines (CPP12), as a class in the unnamed
// namespace, so that two copies of the part never clash, keeps its methods to one file;
// what needs none of its state lives in the headers beside it.
#include "Graph.h"

#include "runtime/View.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

// A box is its border, then its fill, and one holding boxes is that over its rows, then
// its sides and its bottom round the boxes inside it, which show through; then a mark a
// port, where its curves land.
constexpr std::uint32_t box_rects = 5;
constexpr std::uint32_t mark_room = 8; // marks a box, on average
constexpr std::uint32_t select_rects = 4; // the select box's edges
constexpr std::uint32_t corner_rects = 8; // two a corner of the box the pointer is over
constexpr std::uint32_t rect_room =
    (box_rects + mark_room) * node_room + select_rects + corner_rects;
// The area, whose lines its shaders draw.
constexpr std::uint32_t ground_room = 1;
constexpr std::uint32_t label_room = 4096;      // the names, rows and ports together
constexpr std::uint32_t character_room = 32768; // their characters
// A curve a line and a corner of each reader's way, as many as its levels have.
constexpr std::uint32_t curve_room = 8192;
constexpr std::int32_t border_width = 1;        // in pixels
constexpr std::int32_t selected_width = 2;      // a selected box's border, in pixels
constexpr float mark_size = 6.0f;               // a port's mark, in pixels at zoom 1
// A cubic's handles, of its radius, that keep it on a quarter circle.
constexpr float quarter = 0.5523f;
constexpr float drag_least = 3.0f;              // pixels a press moves before a box follows
// A corner a drag pulls: how far in from it a press takes it, never past a quarter of the
// box, so a small box still moves, and how far out, past the box's edges, so a press
// near the corner needs no aim; and its mark's arms, in pixels.
constexpr float corner_reach = 12.0f;
constexpr float corner_out = 6.0f;
constexpr float corner_arm = 12.0f;
constexpr float corner_width = 2.0f;
// Zoomed in this near, the graph shows the inside of the box under the pointer, however
// many boxes home made room for, so a box is gone into as close at
// every level; zoomed out to this share of as near as home is, as far as the wheel goes
// here, a level up.
constexpr float enter_zoom = 3.0f;
constexpr float leave_share = 0.25f;
// The steps of the wheel a level gone up to stops short of going in again, so a little
// more zoom goes back where the zooming out began.
constexpr int focus_steps = 2;
constexpr std::string_view enter_key = "."; // into the box selected, or under the pointer
constexpr std::string_view leave_key = ","; // up a level
constexpr std::string_view every_key = "a"; // with control, every box selected
constexpr std::string_view undo_key = "z";  // with control the last change of where the
                                            // boxes are taken back, with shift too again
constexpr std::string_view title = "graph";


// What of a Rect lies inside another, in its colors; no extent when nothing does.
VP_VIEW::Rect cut(const VP_VIEW::Rect &rect, const VP_VIEW::Rect &by) {
  const glm::ivec2 to = rect.offset + glm::ivec2(rect.extent);
  const glm::ivec2 by_to = by.offset + glm::ivec2(by.extent);
  const glm::ivec2 from{std::max(rect.offset.x, by.offset.x),
                        std::max(rect.offset.y, by.offset.y)};
  const glm::ivec2 end{std::min(to.x, by_to.x), std::min(to.y, by_to.y)};
  VP_VIEW::Rect shown = rect;
  shown.offset = from;
  shown.extent =
      end.x <= from.x || end.y <= from.y ? glm::uvec2(0) : glm::uvec2(end - from);
  return shown;
}

// Whether two Rects share a pixel.
bool touches(const VP_VIEW::Rect &rect, const VP_VIEW::Rect &by) {
  return cut(rect, by).extent.x != 0;
}

bool inside(const VP_VIEW::Rect &area, glm::vec2 point) {
  return VP_VIEW::contains(area, point);
}

// Whether a text holds the words, whatever their case, as the graph's find takes them.
bool mentions(std::string_view text, std::string_view words) {
  const auto same = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  return !std::ranges::search(text, words, same).empty();
}

// Whether a line can name a box, as select takes its words: what lies outside, named with
// a blank, it cannot, nor a file or a folder whose name holds one.
bool nameable(std::string_view name) {
  return name.find_first_of(" \t\r") == std::string_view::npos;
}

// Whether a menu kind is a level's ground, which the graph's ground() names, as no box
// is.
bool background(std::string_view kind) {
  return kind == "ground" || kind == "inside" || kind == "level";
}

// The view hosting the one named, found from the view vulpen started with down the views
// each hosts, by the names lines address them with; none for that one, or one no view
// hosts.
template <class Port>
std::optional<std::string> host_of(Port &port, std::string_view name) {
  std::vector<std::string> hosts{std::string()};
  for (std::size_t at = 0; at < hosts.size() && !name.empty(); ++at)
    if (const VP::View *const host = port.view(hosts[at]))
      for (const VP::Child &child : host->children) {
        if (child.name == name)
          return hosts[at];
        hosts.push_back(child.name);
      }
  return std::nullopt;
}

// A path as a line sent takes it, from where vulpen started, as the prompt names its
// folder, and ./ before one at the top, which open would take for a file the view holds
// by name; the menus name it from where the prompt is, in the lines they hand the
// terminal.
std::string typed_path(const std::filesystem::path &path) {
  const std::filesystem::path typed = path.lexically_proximate(std::filesystem::current_path());
  return (typed.has_parent_path() ? typed : "." / typed).generic_string();
}

// The folders of a view's level no node names: at its top, but the folders of what its
// nodes share, the hidden ones and its hosted views', or in a node's inside; each shows as
// a box, which a right press makes a node, as every folder in a view is a node's (RV08).
// At its top too the files beside its manifest, which no node holds, as new leaves
// one there, each a box a press opens, as the manifest's; in a node's inside every file
// is the node's. A folder's name ends with /, as the file port lists it. Read as the
// level is built, as a node remove keep, which leaves the folder, builds it anew.
std::vector<std::string>
strays_of(VP::FilePort &files, const VP::View &edited, std::string_view inside) {
  std::string path(inside);
  std::ranges::replace(path, '.', '/');
  const std::filesystem::path folder = edited.file.parent_path() / path;
  std::vector<std::string> strays;
  std::vector<std::string> names;
  try {
    names = files.list(folder.lexically_normal().string());
  } catch (const std::runtime_error &) {
    return strays; // a folder it cannot list holds none it shows
  }
  for (const std::string &name : names) {
    if (name.starts_with('.'))
      continue;
    if (!name.ends_with('/')) {
      if (inside.empty() && name != VP::manifest_file && !name.ends_with('~'))
        strays.push_back(name);
      continue;
    }
    const std::string bare = name.substr(0, name.size() - 1);
    const std::string node = inside.empty() ? bare : std::format("{}.{}", inside, bare);
    if ((inside.empty() && (bare == VP::contracts_folder || bare == VP::shared_folder ||
                            std::ranges::find(edited.children, bare, &VP::Child::name) !=
                                edited.children.end())) ||
        std::ranges::find(edited.nodes, node, &VP::Node::name) != edited.nodes.end())
      continue;
    strays.push_back(name);
  }
  return strays;
}

// Whether a path names a folder, as its folder lists it with a / after its name, as the
// editor part tells one, which open hands the graph rather than the editor.
bool folder_named(VP::FilePort &files, const std::filesystem::path &path) {
  const std::vector<std::string> names = files.list(path.parent_path().string());
  return path.filename().empty() ||
         std::ranges::find(names, path.filename().string() + '/') != names.end();
}

// The view vulpen hosts whose folder this is, by the name lines address it with, empty
// for the one vulpen started with; none for a folder no view is.
template <class Port>
std::optional<std::string> view_in(Port &port, const std::filesystem::path &folder) {
  std::vector<std::string> views{std::string()};
  for (std::size_t at = 0; at < views.size(); ++at)
    if (const VP::View *const view = port.view(views[at])) {
      if (view->file.parent_path().lexically_normal() == folder.lexically_normal())
        return views[at];
      for (const VP::Child &child : view->children)
        views.push_back(child.name);
    }
  return std::nullopt;
}

// The line of the ground nearest a place of the graph, along an axis.
std::int32_t line_near(float at) {
  return static_cast<std::int32_t>(std::lround(at / line_spacing)) * line_spacing;
}

// The line of the ground past a place of the graph along an axis, the way given.
std::int32_t line_past(std::int32_t at, int way) {
  const float lines = static_cast<float>(at) / static_cast<float>(line_spacing);
  return static_cast<std::int32_t>(way > 0 ? std::floor(lines) + 1.0f
                                           : std::ceil(lines) - 1.0f) *
         line_spacing;
}

// The way an arrow key points along each axis; none for any other key.
glm::ivec2 arrow_of(std::string_view key) {
  return {key == "right" ? 1 : key == "left" ? -1 : 0,
          key == "down" ? 1 : key == "up" ? -1 : 0};
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
  _shows = node.sets("shows") ? node.param<std::string>("shows") : std::string();
  _relations = &node.input<VP_VIEW::Relations>("relations");
  _layers = node.sets("relations") ? layers_of(node.param<std::string>("relations"))
                                   : every_layer;
  _buttons = &node.output<VP_VIEW::Items>("layers");
  _tip = &node.output<VP_VIEW::Items>("tip");
  _tip_place = &node.output<VP_VIEW::Rect>("tip-place");
  buttons(*_buttons, _layers, _view, _name);
  _heating.heat = node.sets("heat") ? heat_of(node.param<std::string>("heat")) : Heat{};
  heat_buttons(*_buttons, _heating.heat, _view, _name);
  _heating.stale();
  _manhattan = !node.sets("manhattan") || manhattan_of(node.param<std::string>("manhattan"));
  manhattan_button(*_buttons, _manhattan, _view, _name);
  _select = node.command("select [<node>...]",
                         "selects nodes of the view the graph shows, views it hosts, or "
                         "files and folders it shows, in place of those selected; with "
                         "none, none");
  _selected = node.command(
      "selected", "lists the nodes, views, files and folders the graph has selected");
  _find = node.command("select find <value>...",
                       "selects the next box the graph shows, after the one selected "
                       "last, whose name or a row holds the words, joined by single "
                       "blanks, whatever their case, as its find bar finds them; the "
                       "graph stays where it is");
  _cd = node.command("cd [<folder>]",
                     "goes into a box the graph shows, a node's, a view's or a "
                     "folder's, as a second press on it does, or into a folder by its "
                     "path; .. goes up a level, as , does, and / to the top; with none, "
                     "says where the graph is, as the terminal's prompt shows it");
  _place = &node.output<VP_VIEW::Place>("place");
  _arrange = node.command("arrange",
                          "lays out the boxes the graph shows as it does alone, letting "
                          "go of where they were put and the sizes they were given "
                          "there; control with z in the graph takes it back");
  _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
  _labels = node.upload<VP_VIEW::Label>("labels", label_room);
  _characters = node.upload<VP_VIEW::Character>("characters", character_room);
  _curves = node.upload<VP_VIEW::Curve>("curves", curve_room);
  _tabs = &node.output<VP_VIEW::Items>("tabs");
  _ground = node.upload<VP_VIEW::Rect>("ground", ground_room);
  _pan = node.value<glm::vec2>("pan");
  _zoom = node.value<float>("zoom");
  _spacing = node.value<float>("spacing");
}

// The current view, where the terminals' lines go, the one vulpen started with too, or the
// folder the relations part reads in its place, until a view is switched to. The keys
// once it is known, and before it is read for the frame, as a removal changes it.
void Graph::cook(VP::Cook &frame) {
  _lines = frame.commands().done();
  const std::string shown(frame.commands().current());
  const bool folder = !_relations->folder.empty();
  if (_tabs->empty() || shown != _shown) {
    if (!_tabs->empty() && folder)
      show_view(frame.commands());
    show(shown);
  }
  if (const VP::View *const view = frame.view(shown); folder || view)
    arrange(frame,
            folder ? std::filesystem::path(_relations->top) : view->file.parent_path());
  else
    arrange(frame, {});
  if (_typed->focus != _name)
    _control = false; // let go while another part had the focus, which never came here
  for (const VP::Event &event : _typed->to(_name))
    if (event.kind == VP::Event::Kind::key && event.name == "control")
      _control = event.down;
    else if (event.kind == VP::Event::Kind::key && event.name == "shift")
      _shift = event.down; // as the window says too, in this order for a key with it
    else if (event.kind == VP::Event::Kind::key && event.down)
      key(frame, event.name);
  const VP::View *const edited = folder ? &_relations->tree : frame.view(shown);
  const VP::View *const flat = folder ? &_relations->tree : frame.unfolded(shown);
  _hosted.clear();
  for (std::size_t child = 0; edited && !folder && child < edited->children.size(); ++child)
    _hosted.push_back(frame.view(edited->children[child].name));
  if (edited != _built_edited || flat != _built_flat || _inside != _built_inside ||
      _relations->folder != _built_folder || _relations->changes != _built_changes ||
      _hosted != _built_hosted)
    build(frame.files(), edited, flat);
  if (!std::exchange(_tab_named, true))
    name_tab(frame);
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
  // Home again as the area changes size while at home, as the window is maximized.
  if (room && (std::exchange(_homing, false) || (_at_home && _area->extent != _homed))) {
    home();
    _home_zoom = _pan_zoom.zoom;
    _at_home = _left.empty();
    if (!_left.empty())
      focus(std::exchange(_left, {}));
  }
  // A press may send a line that edits the view shown, as open does the ide's own, or
  // closes it; the boxes, built from the view before, stay until the next frame.
  for (const VP::Event &event : frame.input().events())
    follow(frame, room, event);
  lay_out(); // as a drag placed a box
  frame.set(_pan, _pan_zoom.pan);
  frame.set(_zoom, _pan_zoom.zoom);
  frame.set(_spacing, static_cast<float>(line_spacing));
  if (room) {
    _hovered = under(_pointer);
    _hovered_more = more_under();
    curves_under(_pointer);
    _heating.weigh(frame,
                   _boxes,
                   _shown,
                   folder ? std::string_view(_relations->top) : std::string_view());
    write(frame, cell);
    write_tip();
    return;
  }
  // No room, no work: nothing written draws nothing.
  _tip->clear();
  frame.write(_ground, 0);
  frame.write(_rects, 0);
  frame.write(_labels, 0);
  frame.write(_characters, 0);
  frame.write(_curves, 0);
}

// On the view the graph shows, the current one, as the terminals' lines go to it, so a
// line to any other is refused rather than acting where nobody looks; arrange acts on
// what the graph shows, whichever view its line addressed, as its button sends it.
void Graph::command(VP::Call &call) {
  if (call.is(_arrange)) {
    arrange_anew(call.files());
    return;
  }
  if (call.is(_find)) {
    find(call);
    return;
  }
  if (call.is(_cd)) {
    cd(call);
    return;
  }
  const std::string shown(call.commands().current());
  if (call.view(shown) != &call.view())
    throw std::runtime_error(std::format(
        "the graph shows {}, so its lines go there, as {}: select",
        shown.empty() ? std::string_view("the view vulpen started with") : shown,
        shown));
  if (_selected_in != shown) // a selection in a view hosted before
    _selection.clear();
  const VP::View &view = call.view();
  // A node, a view it hosts, or a box shown that a line can name, as a file's or a
  // folder's.
  const auto boxed = [&](std::string_view name) {
    const auto &children = view.children;
    return std::ranges::find(view.nodes, name, &VP::Node::name) != view.nodes.end() ||
           std::ranges::find(children, name, &VP::Child::name) != children.end() ||
           std::ranges::any_of(_boxes.boxes, [&](const Box &box) {
             return box.name == name && nameable(box.name);
           });
  };
  if (call.is(_select)) {
    std::vector<std::string> chosen;
    for (const std::string_view name : call.arguments()) {
      if (!boxed(name))
        throw std::runtime_error(std::format(
            "no node, view it hosts or box the graph shows is named {}; ls lists the "
            "nodes",
            name));
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

// delete or backspace removes what is selected; an arrow moves it a line of the ground
// that way; escape lets go of it, and with none of the focus, as the editor and the
// terminal do; control with a selects every box; control with z takes back the last
// change of where the boxes are, or the line a person ran after it, and with shift too
// does it again. The home key brings the graph home once the frame has laid the boxes
// out.
void Graph::key(VP::Cook &frame, std::string_view key) {
  const bool chosen = _selected_in == _shown && !_selection.empty();
  if ((key == "delete" || key == "backspace") && chosen) {
    erase(frame);
  } else if (const glm::ivec2 way = arrow_of(key); way != glm::ivec2(0) && chosen) {
    nudge(frame, way);
  } else if (key == undo_key && _control) {
    undo(frame, _shift);
  } else if (key == "escape" && chosen) {
    frame.commands().send(std::format("{}: select", _shown));
  } else if (key == "escape") {
    frame.commands().send("focus");
  } else if (key == PanZoom::home_key) {
    _left.clear();
    _homing = true;
  } else if (key == leave_key) {
    leave(frame);
    _left.clear(); // the level up shows from home, as h shows it; the wheel's, in focus
  } else if (key == enter_key) {
    go_in(frame);
  } else if (key == every_key && _control) {
    std::vector<std::string> every;
    for (const Box &box : _boxes.boxes)
      every.push_back(box.name);
    select(frame.commands(), every);
  }
}

// The box selected last that has an inside, or the one under the pointer; what lies
// outside, which has none, goes up a level only by , or the wheel.
void Graph::go_in(VP::Cook &frame) {
  for (auto name = _selection.rbegin(); name != _selection.rend() && _selected_in == _shown;
       ++name)
    if (const auto found = std::ranges::find(_boxes.boxes, *name, &Box::name);
        found != _boxes.boxes.end() && found->role &&
        enter(frame, static_cast<std::size_t>(found - _boxes.boxes.begin())))
      return;
  if (const std::size_t at = hit(_pointer); at != no_box && _boxes.boxes[at].role)
    enter(frame, at);
}

// The box left shown in the middle of the pointer, or of the area with the pointer out
// of it, as near as two steps of the wheel short of going into it, but never farther
// than home. Home stays as near as home was, so the steps to go up again stay as many.
void Graph::focus(std::string_view left) {
  const bool folders = !_relations->folder.empty();
  for (std::size_t at = 0; at < _layout.placed().size(); ++at) {
    if (folders ? folder_of(at) != std::filesystem::path(left).lexically_normal()
                : _boxes.boxes[at].name != left)
      continue;
    const Placed &placed = _layout.placed()[at];
    _pan_zoom.zoom =
        std::max(std::min(enter_zoom, PanZoom::most) /
                     std::pow(PanZoom::step, static_cast<float>(focus_steps)),
                 _home_zoom);
    const glm::vec2 middle =
        inside(*_area, _pointer)
            ? _pointer
            : glm::vec2(_area->offset) + glm::vec2(_area->extent) / 2.0f;
    const glm::vec2 centre = glm::vec2(placed.at) + glm::vec2(placed.extent) / 2.0f;
    _pan_zoom.pan = middle - glm::vec2(_area->offset) - centre * _pan_zoom.zoom;
    return;
  }
}

std::filesystem::path Graph::folder_of(std::size_t box) const {
  return folder_path(_relations->top, _boxes.boxes[box].name);
}

// The tab names the view, which shows from home. A selection in another view goes, and
// the boxes are built anew, as another view may have taken the place in memory of the
// one they were built from.
void Graph::show(std::string_view name) {
  _shown = name;
  _heating.stale();
  if (_selected_in != name)
    _selection.clear();
  _tab_named = false;
  _homing = true;
  _grab.reset();
  _select_from.reset();
  _built_edited = nullptr;
  _built_flat = nullptr;
}

// What the boxes shown keep in an .ini: a view's, beside its manifest, or a folder's, at
// the top of its repository, as `.vulpen/` there, which every folder of the repository
// shares, its boxes named by their paths; read again as what shows moves to another.
// An .ini that holds a mistake is named in the log and kept as it is, so no save writes
// over what a person typed; the graph then lays the boxes out from the graph alone.
void Graph::arrange(VP::Cook &frame, const std::filesystem::path &beside) {
  const std::filesystem::path folder = beside.empty() ? beside : beside / kept_folder;
  if (const std::string ini = folder.empty() ? std::string() : (folder / kept_file).string();
      ini != _ini)
    _ini = ini;
  else
    return;
  _arranged = {};
  _refused = false;
  _raised.clear();
  _laid = false;
  const std::vector<std::string> kept =
      folder.empty() ? std::vector<std::string>{} : frame.files().list(folder.string());
  if (std::ranges::find(kept, kept_file) == kept.end())
    return;
  try {
    _arranged = arranged_of(frame.files().read(_ini));
  } catch (const std::runtime_error &mistake) {
    _arranged = {};
    _refused = true;
    frame.log(VP::Level::error,
              std::format("{}; the graph lays the boxes out from the graph alone, and "
                          "keeps the file as it is",
                          mistake.what()));
  }
}

// A box pressed may be one the boxes no longer hold. A level shown anew starts at home,
// and one still at home goes there again, as the relations arriving after its boxes may
// widen it or order its columns anew. A folder always shows its own inside, as the
// relations part reads a folder a level at a time; the paths past it are named from the
// folder holding it, and a view's from its folder.
void Graph::build(VP::FilePort &files, const VP::View *edited, const VP::View *flat) {
  const std::string &folder = _relations->folder;
  if (_inside != _built_inside || folder != _built_folder || _at_home)
    _homing = true;
  _built_edited = edited;
  _built_flat = flat;
  _built_inside = _inside;
  _built_folder = folder;
  _built_changes = _relations->changes;
  _built_hosted = _hosted;
  _heating.stale();
  _boxes = {};
  _hovered_curves.clear();
  if (edited && flat && (folder.empty() || !flat->nodes.empty())) {
    _base = folder.empty() ? edited->file.parent_path()
                           : std::filesystem::path(folder).parent_path();
    _boxes = boxes_of(*edited,
                      *flat,
                      folder.empty() ? _inside : flat->nodes.front().name,
                      _relations->relations,
                      _hosted,
                      _base,
                      !folder.empty(),
                      folder.empty() ? strays_of(files, *edited, _inside)
                                     : std::vector<std::string>());
  }
  draw_order(_boxes, _raised, _order);
  _tab_named = false;
  place(edited, flat);
  _laid = false;
  _grab.reset();
}

// The view again, in place of the folder the relations part reads.
void Graph::show_view(VP::CommandPort &commands) const {
  commands.send(
      std::format("{}: param unset {} folder", _relations->view, _relations->node));
}

// A folder's inside, which the relations part reads, in place of what shows.
void Graph::show_folder(VP::CommandPort &commands,
                        const std::filesystem::path &folder) const {
  commands.send(std::format("{}: param set {} folder {}",
                                    _relations->view,
                                    _relations->node,
                                    folder.generic_string()));
}

// The tab names where the graph is, a word a level, joined as one tab, a press on a word
// going there as cd and the inside param go: its own word for the view vulpen started
// with, then the views hosting the view shown, that view and the nodes whose inside
// shows, or the folders down to the one shown, from the folder holding its repository's
// top, so the repository's name leads them. Where it is takes no line.
void Graph::name_tab(VP::Cook &frame) {
  _tabs->clear();
  const auto word = [&](std::string label, std::string line) {
    _tabs->push_back({.label = std::move(label),
                      .command = std::move(line),
                      .current = true,
                      .joined = !_tabs->empty()});
  };
  const auto cd = [](const std::filesystem::path &folder) {
    return std::format("cd {}", typed_path(folder));
  };
  const VP::View *const top = frame.view("");
  word(std::string(title), top ? cd(top->file.parent_path()) : std::string());
  if (const std::filesystem::path folder(_relations->folder); !folder.empty()) {
    std::filesystem::path at = std::filesystem::path(_relations->top).parent_path();
    for (const std::filesystem::path &name : folder.lexically_relative(at)) {
      at /= name;
      word(std::format("{}{}", _tabs->size() == 1 ? " - " : "/", name.generic_string()),
           cd(at));
    }
  } else {
    std::vector<std::string> views; // the view shown, after the views hosting it
    for (std::optional<std::string> view = _shown; view && !view->empty();
         view = host_of(frame, *view))
      views.insert(views.begin(), *view);
    for (const std::string &view : views)
      if (const VP::View *const hosted = frame.view(view))
        word(std::format(" - {}", view), cd(hosted->file.parent_path()));
    const std::string_view scope = _boxes.scope;
    for (std::size_t from = 0; from < scope.size();) {
      const std::size_t end = std::min(scope.find('.', from), scope.size());
      word(std::format("{}{}", from == 0 ? " - " : ".", scope.substr(from, end - from)),
           std::format("{}: param set {} inside {}", _view, _name, scope.substr(0, end)));
      from = end + 1;
    }
  }
  _tabs->back().command.clear();
}

// Every box in the middle of the area, as near as lets them all show with a margin round
// them, never nearer than zoom 1, so text keeps its size where it can; where even the
// least zoom cannot hold them, from their left or their top, at the least. Whole pixels
// from the area's top left, so text stays as sharp as the font drew it.
void Graph::home() {
  _pan_zoom.home();
  _homed = _area->extent;
  const std::span<const Placed> placed = _layout.placed();
  if (placed.empty())
    return;
  glm::ivec2 low = placed.front().at;
  glm::ivec2 high = low;
  for (const Placed &box : placed) {
    low = glm::min(low, box.at);
    high = glm::max(high, box.at + glm::ivec2(box.extent));
  }
  const glm::vec2 room(_area->extent);
  const glm::vec2 margin(margin_of());
  const glm::vec2 size(high - low);
  const glm::vec2 fits = (room - 2.0f * margin) / size;
  _pan_zoom.zoom = std::clamp(std::min(fits.x, fits.y), PanZoom::least, 1.0f);
  _pan_zoom.pan = glm::round(glm::max((room - size * _pan_zoom.zoom) / 2.0f, margin) -
                             glm::vec2(low) * _pan_zoom.zoom);
}

// In the order the events came, so a press is tested where the pointer was then. With
// no view in its area it takes no press and no wheel, only where the pointer went.
void Graph::follow(VP::Cook &frame, bool room, const VP::Event &event) {
  if (event.kind == VP::Event::Kind::pointer) {
    if (_grab && room) {
      drag(event.at);
    } else if (_zooming && room) {
      turn(frame, (event.at.x - _pointer.x) / PanZoom::drag_step, *_zooming);
    } else if (_dragging) {
      _pan_zoom.pan += event.at - _pointer;
      _at_home = false;
    }
    _pointer = event.at;
  } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
    if (!event.down)
      release(frame, room);
    else if (room && inside(*_area, _pointer))
      press(frame, event.presses);
  } else if (event.kind == VP::Event::Kind::button && event.name == "right") {
    if (event.down && room && inside(*_area, _pointer) && !_grab && !_dragging)
      context(frame);
  } else if (event.kind == VP::Event::Kind::button && event.name == "middle") {
    if (!event.down)
      _zooming.reset();
    else if (room && inside(*_area, _pointer) && !_grab && !_dragging && !_select_from)
      _zooming = _pointer;
  } else if (event.kind == VP::Event::Kind::wheel && room && inside(*_area, _pointer)) {
    turn(frame, event.turn.y, _pointer);
  } else if (event.kind == VP::Event::Kind::key && event.name == "shift") {
    _shift = event.down;
    if (_grab && room)
      drag(_pointer); // locked to an axis, or let go of it, where the pointer is
  } else if (event.kind == VP::Event::Kind::focus && !event.down) {
    // Let go while the window had no focus, which no event says.
    _shift = false;
  }
}

// It takes the focus. On a box, the box drawn last over the others: a second press opens
// the file of the row pressed, or goes into the box's inside, or up a level from what lies
// outside. Any box, a node's, a view's, a file's, a folder's or what lies outside: shift
// adds it to the selection, or it is selected, unless it is already, and grabbed with
// every box selected, so a drag moves them all; one selected already shift takes out as
// it is let go where it was, so a drag with shift moves them too. On the ground, with
// shift, a select box from there to the pointer until the button is let go; without, the
// selection goes, and the graph follows the pointer until then.
void Graph::press(VP::Cook &frame, std::uint32_t presses) {
  if (_typed->focus != _name)
    frame.commands().send(std::format("focus {}", _name));
  const bool chosen_here = _selected_in == _shown && !_selection.empty();
  const Under pressed_at = under(_pointer);
  if (pressed_at.corner) {
    grab_corner(frame, pressed_at);
    return;
  }
  if (const std::size_t at = pressed_at.box; at != no_box) {
    if (presses == 2 && (open(frame, at) || enter(frame, at)))
      return;
    const Box &pressed = _boxes.boxes[at];
    std::vector<std::string> chosen;
    if (_selected_in == _shown)
      chosen = _selection;
    const bool was = selected(pressed.name);
    if (!was) {
      if (!_shift)
        chosen.clear();
      chosen.push_back(pressed.name);
      select(frame.commands(), chosen);
    }
    _grab = Grab{.box = at,
                 .from = _pointer,
                 .shifted = _shift,
                 .takes_out = _shift && was,
                 .at = _layout.placed()[at].at};
    for (std::size_t box = 0; box < _boxes.boxes.size(); ++box)
      if (selected(_boxes.boxes[box].name))
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

// Of what the pointer is on: a box, or a row of it naming a file or a folder; a curve, a
// connection's or a relation's, which names the files at its ends; or the ground. A box
// selected among others names every box selected of its kind, whose lines run once for
// each. A name a line cannot carry, as one holding a blank, opens the ground's instead.
void Graph::context(VP::Cook &frame) {
  std::string kind;
  std::vector<std::string> names;
  if (const std::size_t at = under(_pointer).box; at != no_box) {
    std::string name;
    std::tie(kind, name) = menu_of(frame, at, true);
    names.push_back(std::move(name));
    if (_selected_in == _shown && _selection.size() > 1 && selected(_boxes.boxes[at].name))
      for (const std::string &chosen : _selection)
        if (const auto box = std::ranges::find(_boxes.boxes, chosen, &Box::name);
            box != _boxes.boxes.end())
          if (auto [other, named] = menu_of(
                  frame, static_cast<std::size_t>(box - _boxes.boxes.begin()), false);
              other == kind && std::ranges::find(names, named) == names.end())
            names.push_back(std::move(named));
  } else if (!_hovered_curves.empty() && _built_flat) {
    const End &end = _boxes.ends[_hovered_curves.front().second];
    if (end.layer == VP_VIEW::Layer::flow && end.wire < _built_flat->connections.size()) {
      kind = "connection";
      names.push_back(_built_flat->connections[end.wire].name);
    } else if (end.layer != VP_VIEW::Layer::flow) {
      kind = "relation";
      for (const auto &[box, row] : {std::pair(end.from, end.from_row),
                                    std::pair(end.to, end.to_row)})
        if (const std::filesystem::path &opens =
                row < _boxes.boxes[box].rows ? rows_in(_boxes, box)[row].opens
                                             : _boxes.boxes[box].opens;
            !opens.empty())
          names.push_back(typed_path(opens));
    }
  }
  std::erase_if(names, [](const std::string &name) { return !nameable(name); });
  if (names.empty()) {
    std::string name;
    std::tie(kind, name) = ground(frame);
    if (name.empty() || !nameable(name))
      return;
    names.push_back(std::move(name));
  }
  // A background's menu ends with what the terminal does for the whole project, muted.
  std::string line =
      std::format("menu {}{} for", kind, background(kind) ? " project" : "");
  for (const std::string &name : names)
    line.append(" ").append(name);
  frame.commands().send(line);
}

// What kind of thing a box is, as menu.ini names it, and its name as a line takes it; with
// by_row, the row under the pointer, when it names a file or a folder.
std::pair<std::string, std::string>
Graph::menu_of(VP::Cook &frame, std::size_t at, bool by_row) const {
  const Box &box = _boxes.boxes[at];
  const std::size_t row = by_row ? row_under(at) : title_row;
  if (row != title_row && row < box.rows && !rows_in(_boxes, at)[row].opens.empty()) {
    const std::filesystem::path &opens = rows_in(_boxes, at)[row].opens;
    return {folder_named(frame.files(), opens) ? "folder" : "file", typed_path(opens)};
  }
  if (box.view)
    return {"view", box.name};
  if (box.stray)
    return {"stray", box.name};
  if (box.role == VP_VIEW::role("node")) {
    const VP::View *const view = frame.view(_shown);
    const auto node = view ? std::ranges::find(view->nodes, box.name, &VP::Node::name)
                           : std::vector<VP::Node>::const_iterator();
    const bool dropped = view && node != view->nodes.end() &&
                         node->recipe.find('@') != std::string::npos;
    return {dropped ? "dropped" : "node", box.name};
  }
  if (!box.opens.empty())
    return {folder_named(frame.files(), box.opens) ? "folder" : "file",
            typed_path(box.opens)};
  if (box.role == VP_VIEW::role("folder") || box.role == VP_VIEW::role("hidden"))
    return {"folder", typed_path(folder_of(at))};
  return ground(frame);
}

// The level shown: a folder's, by its path, a node's inside, by the node's name, or a
// view's top, by its folder; none for a view with no folder.
std::pair<std::string, std::string> Graph::ground(VP::Cook &frame) const {
  if (!_relations->folder.empty())
    return {"level", typed_path(_relations->folder)};
  if (!_inside.empty())
    return {"inside", _inside};
  const VP::View *const view = frame.view(_shown);
  if (!view || view->file.empty())
    return {};
  return {"ground", typed_path(view->file.parent_path())};
}

// A corner pressed selects its box as a press on it does, and grabs the corner of it and
// of every other box selected, which a drag pulls alike.
void Graph::grab_corner(VP::Cook &frame, const Under &under) {
  const Box &pressed = _boxes.boxes[under.box];
  if (!selected(pressed.name))
    select(frame.commands(), std::span(&pressed.name, 1));
  _grab = Grab{.box = under.box, .from = _pointer, .corner = under.corner};
  const std::span<const Placed> placed = _layout.placed();
  const auto pull = [&](std::size_t at) {
    _grab->pulled.push_back({.box = at,
                             .offset = placed[at].offset,
                             .at = placed[at].at,
                             .extent = glm::ivec2(placed[at].extent),
                             .least = glm::ivec2(placed[at].least),
                             .natural = glm::ivec2(placed[at].natural)});
  };
  pull(under.box);
  for (std::size_t at = 0; at < placed.size(); ++at)
    if (at != under.box && selected(_boxes.boxes[at].name))
      pull(at);
}

// Once the pointer moved a few pixels, so a press that wobbles is still a press. The
// first drag places every box of the view where it shows, so a box moved moves no other,
// and a node added later takes its column's place. While shift is held, the boxes move
// left and right or up and down alone.
void Graph::drag(glm::vec2 to) {
  Grab &grab = *_grab;
  const std::span<const Placed> placed = _layout.placed();
  if (grab.box >= placed.size() ||
      (!grab.moved && glm::length(to - grab.from) < drag_least))
    return;
  if (grab.corner) {
    pull(to);
    return;
  }
  if (!grab.moved) {
    remember();
    place_all();
  }
  grab.moved = true;
  const glm::vec2 moved = (to - grab.from) / _pan_zoom.zoom;
  // The box grabbed snaps to the ground's lines, the others going as far; with shift,
  // along the axis it went farther on alone, the other as it was.
  glm::ivec2 by{line_near(static_cast<float>(grab.at.x) + moved.x) - grab.at.x,
                line_near(static_cast<float>(grab.at.y) + moved.y) - grab.at.y};
  if (_shift)
    (std::abs(moved.x) >= std::abs(moved.y) ? by.y : by.x) = 0;
  for (const auto &[box, offset] : grab.moving)
    _arranged.places.insert_or_assign(_boxes.boxes[box].name, offset + by);
  _laid = false; // so this frame shows them there
}

// The corner grabbed follows the pointer and the opposite one stays, its edges snapping
// to the ground's lines, or to the size the box's rows need where that lies nearer, and
// a line farther at a time while the box would be smaller than the least any box may
// be; every other box selected changes as much, down to its own least. A size its rows
// need anyway leaves the .ini. A left or a top corner moves the boxes too. The first
// pull places every box where it shows, as a drag does, so a box made larger moves no
// other.
void Graph::pull(glm::vec2 to) {
  Grab &grab = *_grab;
  const glm::bvec2 corner = *grab.corner;
  if (!grab.moved) {
    remember();
    place_all();
  }
  const glm::vec2 by = (to - grab.from) / _pan_zoom.zoom;
  const Pulled &first = grab.pulled.front();
  glm::ivec2 low = first.at;
  glm::ivec2 high = first.at + first.extent;
  for (const int axis : {0, 1}) {
    std::int32_t &edge = corner[axis] ? low[axis] : high[axis];
    const std::int32_t natural =
        corner[axis] ? high[axis] - first.natural[axis] : low[axis] + first.natural[axis];
    const float wanted = static_cast<float>(edge) + by[axis];
    const std::int32_t line = line_near(wanted);
    edge = std::abs(wanted - static_cast<float>(natural)) <
                   std::abs(wanted - static_cast<float>(line))
               ? natural
               : line;
    if (const std::int32_t short_by = first.least[axis] - (high[axis] - low[axis]);
        short_by > 0)
      edge += (corner[axis] ? -1 : 1) * (short_by + line_spacing - 1) / line_spacing *
              line_spacing;
  }
  for (const Pulled &pulled : grab.pulled) {
    glm::ivec2 from = pulled.at + (low - first.at);
    glm::ivec2 end = pulled.at + pulled.extent + (high - first.at - first.extent);
    for (const int axis : {0, 1})
      if (corner[axis])
        from[axis] = std::min(from[axis], end[axis] - pulled.least[axis]);
      else
        end[axis] = std::max(end[axis], from[axis] + pulled.least[axis]);
    const std::string &name = _boxes.boxes[pulled.box].name;
    if (end - from == pulled.natural)
      _arranged.sizes.erase(name);
    else
      _arranged.sizes.insert_or_assign(name, end - from);
    if (corner.x || corner.y)
      _arranged.places.insert_or_assign(name, pulled.offset + (from - pulled.at));
  }
  grab.moved = true;
  _laid = false;
}

// The boxes selected a line of the ground over, the one selected last onto the next line
// that way and the others as far, as a drag takes them; saved at once. Where that box is
// comes from its place, which the presses before it this frame moved, as the boxes are
// laid out again only after the keys.
void Graph::nudge(VP::Cook &frame, glm::ivec2 way) {
  const std::span<const Placed> placed = _layout.placed();
  const auto at_box = [&](std::string_view name) {
    return static_cast<std::size_t>(std::ranges::find(_boxes.boxes, name, &Box::name) -
                                    _boxes.boxes.begin());
  };
  const auto lead = std::ranges::find_if(
      _selection.rbegin(), _selection.rend(), [&](const std::string &name) {
        return at_box(name) < placed.size();
      });
  if (lead == _selection.rend())
    return;
  remember();
  place_all();
  const Placed &laid = placed[at_box(*lead)];
  const glm::ivec2 at = laid.at - laid.offset + _arranged.places.at(*lead);
  glm::ivec2 by{};
  for (const int axis : {0, 1})
    if (way[axis] != 0)
      by[axis] = line_past(at[axis], way[axis]) - at[axis];
  for (const Box &box : _boxes.boxes)
    if (const auto place = _arranged.places.find(box.name);
        place != _arranged.places.end() && selected(box.name))
      place->second += by;
  _laid = false;
  _at_home = false;
  save(frame);
}

// Before a change of where the boxes are: the history of the .ini keeps what was
// arranged, and what was taken back goes, as an editor's redo does.
void Graph::remember() {
  History &history = _histories[_ini];
  history.undone.push_back({_arranged, _lines});
  if (history.undone.size() > undo_room)
    history.undone.erase(history.undone.begin());
  history.redone.clear();
}

// What was arranged before the last change, or with again the change taken back last,
// laid out and saved at once, as the change was; or, when a line a person ran came after
// it, undo or redo of that line, run in the terminal through its type as a key's line is,
// so a node the graph's Delete key took comes back as a box moved does.
void Graph::undo(VP::Cook &frame, bool again) {
  History &history = _histories[_ini];
  std::vector<Placing> &from = again ? history.redone : history.undone;
  std::vector<Placing> &to = again ? history.undone : history.redone;
  const std::size_t lines = frame.commands().done();
  const bool line = again ? from.empty() || from.back().lines > lines
                          : lines > 0 && (from.empty() || from.back().lines < lines);
  if (line) {
    frame.commands().send(again ? "type redo" : "type undo");
    return;
  }
  if (from.empty() || _grab)
    return;
  to.push_back({std::exchange(_arranged, std::move(from.back().arranged)), lines});
  from.pop_back();
  _laid = false;
  _at_home = false;
  save(frame);
}

// Every box where it shows now, unless it was placed already, so a box moved moves no
// other.
void Graph::place_all() {
  const std::span<const Placed> placed = _layout.placed();
  for (std::size_t at = 0; at < placed.size(); ++at)
    _arranged.places.try_emplace(_boxes.boxes[at].name, placed[at].offset);
}

// A box dragged is saved at once. A select box selects what it touches. A box pressed
// and let go where it was stays the one selected, or with shift, one selected before the
// press leaves the selection.
void Graph::release(VP::Cook &frame, bool room) {
  _dragging = false;
  if (const std::optional<glm::vec2> from = std::exchange(_select_from, std::nullopt);
      from && room)
    select_touched(frame.commands(), *from);
  if (!_grab)
    return;
  const Grab grab = *std::exchange(_grab, std::nullopt);
  if (grab.moved) {
    _at_home = false; // so the boxes a person arranged stay where they see them
    save(frame);
    return;
  }
  if (grab.corner)
    return;
  if (!room || grab.box >= _boxes.boxes.size())
    return;
  const Box &pressed = _boxes.boxes[grab.box];
  if (grab.takes_out) {
    std::vector<std::string> chosen = _selection;
    std::erase(chosen, pressed.name);
    select(frame.commands(), chosen);
  } else if (!grab.shifted && _selection.size() > 1) {
    select(frame.commands(), std::span(&pressed.name, 1));
  }
}

// A second press on a row that names a file opens it, as a line typed would: a node's
// file, from its folder in the view or the library, the manifest of the recipe a node
// uses, that of a view where it is hosted from, a file's own box, or a row of what lies
// outside naming what a relation reaches, a folder showing in the graph; a box's name and
// its ports open none. Whether one was opened. A file goes on to the panel its shows param
// names, which shows again if it was hidden, as a person who opens a file looks at it.
bool Graph::open(VP::Cook &frame, std::size_t at) {
  const std::size_t row = row_under(at);
  const std::filesystem::path opened =
      row == title_row ? _boxes.boxes[at].opens
                       : (row < _boxes.boxes[at].rows ? rows_in(_boxes, at)[row].opens
                                                      : std::filesystem::path());
  if (opened.empty())
    return false;
  frame.commands().send(std::format("open {}", opened.generic_string()));
  if (!_shows.empty() && !folder_named(frame.files(), opened))
    frame.commands().send(std::format("{}: panel show {}", _view, _shows));
  return true;
}

// The row of a box under the pointer, among its rows; title_row for its name's, more_row
// for the row standing for those it lists past what shows, and one past them below
// them.
std::size_t Graph::row_under(std::size_t at) const {
  const Placed &placed = _layout.placed()[at];
  const float slot =
      ((_pointer.y - on_screen(glm::vec2(placed.at)).y) / _pan_zoom.zoom -
       static_cast<float>(pad)) /
          static_cast<float>(_laid_cell.y) -
      1.0f;
  if (slot < 0.0f)
    return title_row;
  const auto shown = static_cast<std::size_t>(slot);
  return shown < Layout::shown_rows(placed) ? Layout::row_at(placed, shown)
                                            : _boxes.boxes[at].rows;
}

// The wheel zooms by steps, about the pointer, which stays over the same place of the
// graph, and a middle drag about where it was pressed, a step each PanZoom::drag_step
// pixels it goes sideways, right nearer and left farther; the wheel's turn sideways does
// nothing yet. Zoomed in to three times as near as zoom 1, or as near as it goes, the
// graph shows the inside of the box under that place; zoomed out to a quarter as near as
// home, as far as it goes here, a level up, once the level it left last shows: a turn
// goes on while the level up is read, which would take it up a level each step.
void Graph::turn(VP::Cook &frame, float steps, glm::vec2 about) {
  const float farthest = _home_zoom * leave_share;
  _pan_zoom.wheel(steps, about - glm::vec2(_area->offset), farthest);
  _at_home = false;
  if (steps < 0.0f && _pan_zoom.zoom <= farthest) {
    if (_left.empty())
      leave(frame);
    return;
  }
  if (steps <= 0.0f || _pan_zoom.zoom < std::min(enter_zoom, PanZoom::most))
    return;
  if (const std::size_t at = hit(about); at != no_box && _boxes.boxes[at].role)
    enter(frame, at);
}

// A box's inside, as a second press or the wheel goes into it: a view's, as view switch,
// a node's, empty or not, as the inside param, which a line sets as a press does (V06),
// and a folder's, as the relations part's folder param, or, for a folder a view vulpen
// hosts is, that view; and from the name of what lies outside, which has no role, a
// level up. False for a box with no inside.
template <class Port> bool Graph::enter(Port &port, std::size_t at) {
  const Box &entered = _boxes.boxes[at];
  if (!entered.role) {
    if (row_under(at) != title_row)
      return false;
    leave(port);
    _left.clear(); // from home, as , goes up
    return true;
  }
  _left.clear(); // a level gone into starts at home
  if (entered.view)
    port.commands().send(std::format("view switch {}", entered.name));
  else if (entered.has_inside && !_relations->folder.empty())
    into_folder(port, folder_of(at));
  else if (entered.has_inside)
    port.commands().send(
        std::format("{}: param set {} inside {}", _view, _name, entered.name));
  return entered.view || entered.has_inside;
}

// A folder shown, or, for a folder a view vulpen hosts is, that view; the frame and a
// command both get here, as a press and cd do. The view shown is there already unless a
// folder shows in its place, or a node's inside, whose params are unset only while set,
// as the engine refuses to unset one that is not.
template <class Port>
void Graph::into_folder(Port &port, const std::filesystem::path &folder) {
  if (const std::optional<std::string> view = view_in(port, folder); !view)
    show_folder(port.commands(), folder);
  else if (*view != _shown)
    port.commands().send(view->empty() ? std::string("view switch")
                                       : std::format("view switch {}", *view));
  else if (!_relations->asked.empty())
    show_view(port.commands());
  else if (!_inside.empty())
    port.commands().send(std::format("{}: param unset {} inside", _view, _name));
}

// Out of the node whose inside shows, to the inside of the node holding it, or the top;
// out of a view's top to the view hosting it, as view switch shows it, and out of the top
// of the view vulpen started with to the folder holding its folder, where its folder is a
// box; out of a folder to the one holding it, up to the top of its repository. The level
// shown next shows the box left in focus, unless the caller lets go of it. Once a line
// left a node's inside, the boxes show it still until the next frame builds them, so a
// wheel turning on leaves it once, as its inside param, which it keeps as the line set it,
// differs from the inside they show.
template <class Port> void Graph::leave(Port &port) {
  if (!_built_flat)
    return;
  if (const std::filesystem::path folder(_relations->asked); !folder.empty()) {
    if (folder != std::filesystem::path(_relations->top) && folder != folder.parent_path()) {
      _left = folder.lexically_normal().generic_string();
      show_folder(port.commands(), folder.parent_path());
    }
    return;
  }
  if (_boxes.scope.empty()) {
    const VP::View *const view = port.view(_shown);
    if (const std::optional<std::string> host = host_of(port, _shown)) {
      _left = _shown;
      port.commands().send(host->empty() ? std::string("view switch")
                                         : std::format("view switch {}", *host));
    } else if (view && !view->file.empty()) {
      _left = view->file.parent_path().lexically_normal().generic_string();
      show_folder(port.commands(), view->file.parent_path().parent_path());
    }
    return;
  }
  if (_inside != _boxes.scope)
    return;
  _left = _boxes.scope;
  const std::size_t holder = parent_of(_built_flat->nodes, _left);
  _inside = holder == no_box ? std::string() : _built_flat->nodes[holder].name;
  port.commands().send(
      _inside.empty()
          ? std::format("{}: param unset {} inside", _view, _name)
          : std::format("{}: param set {} inside {}", _view, _name, _inside));
}

bool Graph::selected(std::string_view node) const {
  return _selected_in == _shown &&
         std::ranges::find(_selection, node) != _selection.end();
}

// As a line, so a press selects as a script or an agent does (V06); a box no line can
// name, as what lies outside, joins the selection after it, raised as select raises the
// others, for the eye alone, as no line reaches it.
void Graph::select(VP::CommandPort &commands, std::span<const std::string> nodes) {
  std::string line = std::format("{}: select", _shown);
  for (const std::string &node : nodes)
    if (nameable(node))
      line.append(" ").append(node);
  commands.send(line);
  for (const std::string &node : nodes)
    if (!nameable(node) && _selected_in == _shown) {
      raise(node);
      _selection.push_back(node);
    }
}

// Each box the select box touches joins the selection, and each selected already leaves
// it, as shift with a press adds one or takes it out; the select box is cut at the area,
// as it shows, so only what shows of a box is touched, and of a frame its rows.
void Graph::select_touched(VP::CommandPort &commands, glm::vec2 from) {
  std::vector<std::string> chosen;
  if (_selected_in == _shown)
    chosen = _selection;
  const VP_VIEW::Rect swept = select_box(from);
  bool touched_any = false;
  for (std::size_t at = 0; at < _layout.placed().size(); ++at) {
    const Box &touched = _boxes.boxes[at];
    if (!touches(solid(at), swept))
      continue;
    touched_any = true;
    if (selected(touched.name))
      std::erase(chosen, touched.name);
    else
      chosen.push_back(touched.name);
  }
  if (touched_any)
    select(commands, chosen);
}

// From the box after the one selected last, or the first, round to that one: the first
// whose name or a row holds the words is selected, as select does, so the next find goes
// on past it; the graph stays where it is, as the find bar's search only marks a box.
void Graph::find(VP::Call &call) {
  std::string wanted;
  for (const std::string_view word : call.arguments())
    wanted.append(wanted.empty() ? "" : " ").append(word);
  const std::vector<Box> &boxes = _boxes.boxes;
  std::size_t start = 0;
  if (_selected_in == _shown)
    for (const std::string &name : _selection)
      if (const auto at = std::ranges::find(boxes, name, &Box::name); at != boxes.end())
        start = static_cast<std::size_t>(at - boxes.begin()) + 1;
  for (std::size_t step = 0; step < boxes.size(); ++step) {
    const std::size_t at = (start + step) % boxes.size();
    const std::span<const BoxRow> rows = rows_in(_boxes, at);
    if (mentions(boxes[at].title, wanted) ||
        std::ranges::any_of(rows, [&](const BoxRow &row) {
          return mentions(row.text, wanted) || mentions(row.written, wanted);
        })) {
      select(call.commands(), std::span(&boxes[at].name, 1));
      return;
    }
  }
  call.reply(std::format("no box the graph shows holds {}", wanted));
}

// With none, where the graph is, from where vulpen started, as a prompt shows it; .. up a
// level, as , goes, from home; / the top of the view shown, or of the repository a folder
// shown is in; a word, a box of the level shown by its name, or in a folder shown a
// folder in it; a path, by either separator the system knows, from where it was typed, a
// folder, or the view whose folder it is, as a second press on its box goes there. Any
// other place is refused, naming why.
void Graph::cd(VP::Call &call) {
  if (call.arguments().empty()) {
    call.reply(std::filesystem::path(_place->folder)
                   .lexically_proximate(std::filesystem::current_path())
                   .generic_string());
    return;
  }
  const std::string_view to = call.arguments().front();
  const std::string &folder = _relations->asked;
  if (to == "..") {
    leave(call);
    _left.clear();
  } else if (to == "/" && !folder.empty()) {
    into_folder(call, _relations->top);
  } else if (to == "/") {
    if (!_inside.empty())
      call.commands().send(std::format("{}: param unset {} inside", _view, _name));
  } else if (const bool typed = std::filesystem::path(to).has_parent_path();
             typed || !folder.empty()) {
    std::filesystem::path path =
        (typed ? std::filesystem::path(to) : std::filesystem::path(folder) / to)
            .lexically_normal();
    if (path.filename().empty())
      path = path.parent_path();
    // A root, as / or C:\, which no folder lists, is one while it lists anything.
    const bool root = path == path.parent_path();
    const std::vector<std::string> names = call.files().list(path.parent_path().string());
    if (root ? names.empty()
             : std::ranges::find(names, path.filename().string() + '/') == names.end())
      throw std::runtime_error(
          std::format("{} is no folder",
                      path.lexically_proximate(std::filesystem::current_path())
                          .generic_string()));
    into_folder(call, path);
  } else {
    const auto named = std::ranges::find_if(_boxes.boxes, [&](const Box &box) {
      return box.role && (box.name == to || box.title == to);
    });
    if (named == _boxes.boxes.end())
      throw std::runtime_error(
          std::format("no box the graph shows is named {}; ls lists what is here", to));
    if (!enter(call, static_cast<std::size_t>(named - _boxes.boxes.begin())))
      throw std::runtime_error(std::format("{} holds nothing to go into", to));
  }
}

// The folder of what shows, as it changes: the folder shown, or the folder of the node
// whose inside shows, which holds its files and the nodes inside it (RV08), or of the
// view.
void Graph::place(const VP::View *edited, const VP::View *flat) {
  std::filesystem::path folder(_relations->folder);
  if (folder.empty() && edited && flat) {
    const auto scope = std::ranges::find(flat->nodes, _boxes.scope, &VP::Node::name);
    const bool top = _boxes.scope.empty() || scope == flat->nodes.end();
    folder = top ? edited->file.parent_path() : scope->folder;
  }
  if (_place->folder != folder.string())
    _place->folder = folder.string();
}

// The boxes shown leave the .ini, where they were put and the sizes they were given, so
// the graph lays them out alone again and shows them from home; the boxes of the other
// levels the .ini keeps stay. Saved at once, and taken back by control with z, as a drag.
void Graph::arrange_anew(VP::FilePort &files) {
  remember();
  for (const Box &box : _boxes.boxes) {
    _arranged.places.erase(box.name);
    _arranged.sizes.erase(box.name);
  }
  _laid = false;
  _homing = true;
  keep(files);
}

// Each box selected goes as a right press on it offers: remove, or for a view it hosts
// view close, pressed in the menus for every box of a kind at once, which hand each line
// to the terminal's type, so it shows the line and its answer and undo takes it back, as
// the menu's own press (V06); a node with files in its folder is refused, naming remove
// force. A node inside another selected goes with that one, and what no menu removes, as
// what lies outside, stays.
void Graph::erase(VP::Cook &frame) {
  std::vector<std::pair<std::string, std::string>> lines; // a kind's, by the kind
  for (const std::string &name : _selection) {
    const auto box = std::ranges::find(_boxes.boxes, name, &Box::name);
    if (box == _boxes.boxes.end() ||
        std::ranges::any_of(_selection, [&](const std::string &other) {
          return name != other && VP::within(name, other);
        }))
      continue;
    const auto [kind, named] =
        menu_of(frame, static_cast<std::size_t>(box - _boxes.boxes.begin()), false);
    if (kind.empty() || background(kind) || !nameable(named))
      continue;
    auto line =
        std::ranges::find(lines, kind, &std::pair<std::string, std::string>::first);
    if (line == lines.end())
      line = lines.insert(lines.end(),
                          {kind, std::format("menu press {} {} for", kind,
                                             kind == "view" ? "view close" : "remove")});
    line->second.append(" ").append(named);
  }
  for (const auto &[kind, line] : lines)
    frame.commands().send(line);
  _grab.reset();
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
// draw before the parts that draw the rest; then each box in the order drawn, so the box
// drawn last shows over the others.
void Graph::write(VP::Cook &frame, glm::uvec2 cell) {
  const std::span<VP_VIEW::Rect> ground = frame.write(_ground);
  ground.front() = {
      .offset = _area->offset, .extent = _area->extent, .role = VP_VIEW::role("border")};
  frame.write(_ground, ground_room);
  const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
  std::uint32_t written = 0;
  for (const std::size_t at : _order)
    write_box(rects, written, at);
  write_corners(rects, written);
  if (_select_from)
    write_select_box(rects, written);
  frame.write(_rects, written); // the used length, which rects draws
  write_labels(frame, cell);
  write_curves(frame);
}

// A border and a fill, the border in the role of what the box is, wider and in the
// accent color while it is selected; one holding boxes is filled over its
// rows alone, and framed below them, so the boxes inside it and the curves between them
// show. Each is cut at the area.
void Graph::write_box(std::span<VP_VIEW::Rect> rects,
                      std::uint32_t &written,
                      std::size_t at) const {
  const Box &shown = _boxes.boxes[at];
  const bool chosen = selected(shown.name);
  const float width = static_cast<float>(chosen ? selected_width : border_width);
  const std::uint32_t edge =
      chosen ? VP_VIEW::role("accent") : shown.role.value_or(VP_VIEW::role("border"));
  const Placed &placed = _layout.placed()[at];
  const bool holds = placed.frames;
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
  const std::vector<Weight> &weights = _heating.weights;
  const Fill fill =
      at < weights.size() ? fill_of(weights[at], _heating.heat, _heating.most) : Fill{};
  VP_VIEW::Rect inside =
      between(from + width, {to.x - width, holds ? below : to.y - width}, fill.role);
  inside.blend_role = fill.blend_role;
  inside.blend = fill.blend;
  add(inside);
  if (holds) {
    add(between({from.x, below}, {from.x + width, to.y}, edge));
    add(between({to.x - width, below}, to, edge));
    add(between({from.x, to.y - width}, to, edge));
  }
  const std::span<const BoxRow> rows = rows_in(_boxes, at);
  const float half = mark_size * _pan_zoom.zoom / 2.0f;
  for (std::size_t row = 0; row < shown.ports; ++row)
    for (const bool right : {false, true})
      if (!(right ? rows[row].written : rows[row].text).empty() &&
          ((right ? rows[row].written_layers : rows[row].read_layers) & _layers) != 0) {
        const glm::vec2 middle = port_of(at, row, right);
        add(between(middle - half, middle + half, VP_VIEW::role("accent")));
      }
}

// The corners of the box the pointer is over, which a drag pulls, while nothing else is
// dragged: two short arms along its edges from each, muted, and in the accent color the
// one under the pointer; on a box selected, whose border is the accent's, in a tint of
// it, and the one under the pointer in the text's color, so they mark its border rather
// than break it.
void Graph::write_corners(std::span<VP_VIEW::Rect> rects, std::uint32_t &written) const {
  if (_hovered.box == no_box || (_grab && !_grab->corner) || _dragging || _select_from)
    return;
  const bool chosen = selected(_boxes.boxes[_hovered.box].name);
  const std::uint32_t arms = chosen ? VP_VIEW::role("highlight") : VP_VIEW::role("muted");
  const std::uint32_t pulled = chosen ? VP_VIEW::role("text") : VP_VIEW::role("accent");
  const VP_VIEW::Rect shown = box(_hovered.box);
  const glm::vec2 low(shown.offset);
  const glm::vec2 high = low + glm::vec2(shown.extent);
  const float arm =
      std::min(corner_arm, static_cast<float>(std::min(shown.extent.x, shown.extent.y)) / 4.0f);
  for (const bool left : {true, false})
    for (const bool top : {true, false}) {
      const std::uint32_t role = _hovered.corner == glm::bvec2(left, top) ? pulled : arms;
      const glm::vec2 at{left ? low.x : high.x, top ? low.y : high.y};
      const glm::vec2 inward{left ? 1.0f : -1.0f, top ? 1.0f : -1.0f};
      for (const glm::vec2 along : {glm::vec2(arm, corner_width), glm::vec2(corner_width, arm)})
        if (const VP_VIEW::Rect mark = cut(between(glm::min(at, at + inward * along),
                                                   glm::max(at, at + inward * along),
                                                   role),
                                           *_area);
            mark.extent.x != 0 && written < rects.size())
          rects[written++] = mark;
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

// A box's name a cell in from its left, between its pads, in the role of what the box is,
// and each row a row below it, in its role: a port read a cell in from the
// left edge, and one written as far in from the right. Each box in the order drawn, its
// rows cut at the boxes drawn after it that it lies under, and where the box ends, a cell
// in from its right and above the boxes inside it, a character at a time, as a person
// makes it smaller: its name losing what it starts with, at a / or a -, after ..., so a
// path keeps its last names, and a row what it ends with, and a port written, right of
// the port read on its row, moving no further left than the rows start.
void Graph::write_labels(VP::Cook &frame, glm::uvec2 cell) {
  Labels written{frame.write(_labels), frame.write(_characters)};
  const auto margin = static_cast<float>(name_margin * cell.x);
  const auto wide = static_cast<float>(cell.x);
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
    const glm::ivec2 extent(placed.extent);
    const glm::vec2 far =
        glm::vec2(placed.at + glm::ivec2(extent.x, placed.frames ? placed.inside : extent.y));
    const float end = far.x - margin;
    const auto room = [&](float right) {
      return cut(between(on_screen({text.x, static_cast<float>(placed.at.y)}),
                         on_screen({right, far.y}),
                         0),
                 *_area);
    };
    fit(_title,
        _boxes.boxes[at].title,
        static_cast<std::size_t>(std::max(end - text.x, 0.0f) / wide));
    write_row(on_screen(text),
              _title,
              _boxes.boxes[at].role.value_or(VP_VIEW::role("accent")),
              cell,
              room(end),
              _over,
              written);
    const std::span<const BoxRow> rows = rows_in(_boxes, at);
    for (std::size_t slot = 0; slot < Layout::shown_rows(placed); ++slot) {
      const std::size_t row = Layout::row_at(placed, slot);
      const glm::vec2 left =
          text + glm::vec2(0.0f, static_cast<float>((slot + 1) * cell.y));
      if (row == more_row) {
        _more.clear();
        std::format_to(std::back_inserter(_more), more_text, placed.more);
        write_row(on_screen(left),
                  _more,
                  VP_VIEW::role("muted"),
                  cell,
                  room(end),
                  _over,
                  written);
        continue;
      }
      const std::string &port = rows[row].written;
      const float right =
          std::max(text.x, end - static_cast<float>(port.size()) * wide);
      if (!rows[row].text.empty())
        write_row(on_screen(left),
                  rows[row].text,
                  rows[row].role,
                  cell,
                  room(port.empty() ? end : right - wide),
                  _over,
                  written);
      if (!port.empty())
        write_row(on_screen({right, left.y}),
                  port,
                  rows[row].role,
                  cell,
                  room(end),
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
                      const VP_VIEW::Rect &room,
                      std::span<const VP_VIEW::Rect> over,
                      Labels &written) const {
  const glm::vec2 size = glm::vec2(cell) * _pan_zoom.zoom;
  const VP_VIEW::Rect whole = cut(
      between(at, at + glm::vec2(static_cast<float>(row.size()) * size.x, size.y), role),
      room);
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
// the left still bends round; or, as the manhattan param asks, straight from its first
// waypoint to its last. A curve goes whole, even with an end out of view: the curves part
// cuts it at the area's edge, which the ground hands it, so a curve to a box dragged out
// runs to the edge.
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
  // The curves under the pointer last, so they show over the others, in the text's color,
  // the nearest last of all.
  const std::size_t count = routes.size() + _hovered_curves.size();
  for (std::size_t drawn = 0; drawn < count; ++drawn) {
    const bool over = drawn >= routes.size();
    const std::size_t end = over ? _hovered_curves[count - 1 - drawn].second : drawn;
    if (end >= routes.size() || (!over && hovered(end)) || !layer_shown(end))
      continue;
    const Route &route = routes[end];
    role = over ? VP_VIEW::role("text")
                : layer_roles[static_cast<std::size_t>(_boxes.ends[end].layer)];
    const std::span<const Waypoint> way = waypoints.subspan(route.first, route.count);
    if (!_manhattan && way.size() > 1) {
      const glm::vec2 from = on_screen(way.front().at);
      const glm::vec2 to = on_screen(way.back().at);
      add(from, to, (to - from) / 3.0f, (to - from) / 3.0f);
      continue;
    }
    for (std::size_t at = 0; at + 1 < way.size(); ++at) {
      const glm::vec2 from = on_screen(way[at].at);
      const glm::vec2 to = on_screen(way[at + 1].at);
      if (way[at].bends) {
        const glm::vec2 reach = bend_reach(from, to);
        add(from, to, reach, reach);
        continue;
      }
      const glm::vec2 along = glm::normalize(to - from);
      const glm::vec2 start = from + along * corner(way, at);
      const float round = corner(way, at + 1);
      const glm::vec2 stop = to - along * round;
      if (stop != start)
        add(start, stop, (stop - start) / 3.0f, (stop - start) / 3.0f);
      if (round > 0.0f) {
        const glm::vec2 next = glm::normalize(on_screen(way[at + 2].at) - to);
        add(stop, to + next * round, along * round * quarter, next * round * quarter);
      }
    }
  }
  frame.write(_curves, written);
}

// While no box lies under the pointer and no drag goes on.
void Graph::curves_under(glm::vec2 point) {
  _hovered_curves.clear();
  if (_hovered.box != no_box || _grab || _dragging || _select_from || !inside(*_area, point))
    return;
  curves_near(_layout.routes(),
              _layout.waypoints(),
              _manhattan,
              _pan_zoom,
              glm::vec2(_area->offset),
              point,
              _hovered_curves);
  std::erase_if(_hovered_curves,
                [&](const auto &near) { return !layer_shown(near.second); });
}

// Whether a curve's layer shows, as the relations param names the layers: a layer hidden
// hides its curves alone, so no box moves as a layer turns.
bool Graph::layer_shown(std::size_t end) const {
  return (_layers & bit_of(_boxes.ends[end].layer)) != 0;
}

bool Graph::hovered(std::size_t end) const {
  return std::ranges::any_of(_hovered_curves, [&](const auto &near) {
    return near.second == end;
  });
}

// The box whose row for the rows past listed_room the pointer is over, while no drag goes
// on; none for none.
std::size_t Graph::more_under() const {
  if (_hovered.box == no_box || _hovered.corner || _grab || _dragging || _select_from)
    return no_box;
  return row_under(_hovered.box) == more_row ? _hovered.box : no_box;
}

// The name or row of the box the pointer is over, whole, where the box cuts it short,
// while no drag goes on; empty for none.
std::string Graph::cut_under() const {
  if (_hovered.box == no_box || _hovered.corner || _grab || _dragging || _select_from)
    return {};
  return cut_short(_boxes,
                   _layout.placed()[_hovered.box],
                   _hovered.box,
                   row_under(_hovered.box),
                   _laid_cell);
}

// The rows of the curve under the pointer, a row each of the curves when several share
// the place, as a lane's do, of a box's rows past listed_room, or of its name or row cut
// short, whole, and where they show.
void Graph::write_tip() {
  _tip->clear();
  *_tip_place = {};
  const glm::ivec2 cell(_font->cell);
  if (_hovered_more < _boxes.boxes.size() && cell.x > 0)
    tell_more(*_tip,
              rows_in(_boxes, _hovered_more)
                  .subspan(_layout.placed()[_hovered_more].list_from +
                               _layout.placed()[_hovered_more].listed,
                           _layout.placed()[_hovered_more].more),
              static_cast<std::size_t>(std::max(
                  static_cast<std::int32_t>(_area->extent.x) / cell.x - tip_margin, 1)));
  else if (std::string whole = cut_under(); !whole.empty())
    _tip->push_back({.label = std::move(whole)});
  else if (_heating.heat.on != Heat::On::none && _hovered.box < _heating.weights.size() &&
           !_hovered.corner)
    tell_cost(*_tip, _boxes.boxes[_hovered.box].title, _heating.weights[_hovered.box]);
  else if (_hovered_curves.size() == 1 && _built_flat)
    tell(*_tip,
         _boxes,
         _hovered_curves.front().second,
         *_built_flat,
         _relations->relations,
         _base);
  else if (_built_flat)
    for (const auto &[near, end] : _hovered_curves)
      if (std::string joined = joins(_boxes, end, *_built_flat); !joined.empty())
        _tip->push_back({.label = std::move(joined)});
  if (_tip->empty() || cell.y <= 0)
    return;
  cut_tip(*_tip,
          static_cast<std::size_t>(
              std::max(static_cast<std::int32_t>(_area->extent.y) / cell.y - 2, 0)));
  *_tip_place = tip_place(*_tip, cell, _pointer, *_area);
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

// A corner of a box, its own or one past a frame's rows, where the box drawn over the
// others lies, or none: from a little outside the box's edges to a little in from them.
Under Graph::under(glm::vec2 point) const {
  if (!inside(*_area, point))
    return {};
  for (std::size_t drawn = _order.size(); drawn-- > 0;) {
    const std::size_t at = _order[drawn];
    if (at >= _layout.placed().size())
      continue;
    const VP_VIEW::Rect shown = box(at);
    const glm::vec2 low(shown.offset);
    const glm::vec2 high = low + glm::vec2(shown.extent);
    const float reach = std::min(
        corner_reach, static_cast<float>(std::min(shown.extent.x, shown.extent.y)) / 4.0f);
    const bool left = point.x < low.x + reach;
    const bool top = point.y < low.y + reach;
    const bool near = point.x >= low.x - corner_out && point.x < high.x + corner_out &&
                      point.y >= low.y - corner_out && point.y < high.y + corner_out;
    if (near && (left || point.x >= high.x - reach) && (top || point.y >= high.y - reach))
      return {at, glm::bvec2(left, top)};
    if (VP_VIEW::contains(cut(solid(at), *_area), point))
      return {at, std::nullopt};
  }
  return {};
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

glm::vec2 Graph::port_of(std::size_t box, std::size_t row, bool right) const {
  const Placed &placed = _layout.placed()[box];
  const float across = right ? static_cast<float>(placed.extent.x) : 0.0f;
  return on_screen(glm::vec2(placed.at) +
                   glm::vec2(across, _layout.row_in(box, row, _laid_cell)));
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
  const auto rows = placed.frames ? placed.inside : static_cast<std::int32_t>(placed.extent.y);
  return between(on_screen(glm::vec2(placed.at)),
                 on_screen(glm::vec2(placed.at) +
                           glm::vec2(static_cast<float>(placed.extent.x),
                                     static_cast<float>(rows))),
                 0);
}

VP_OPERATOR(Graph);
