// The docking part's operator. It runs past 800 lines (CPP12), as a class in the unnamed
// namespace, so that two copies of the part never clash, keeps its methods to one file:
// the panels' moves, hides, stacks, editors, marks and divider all read the docks it
// seats; what needs none of its state lives in the header beside it, Kept.h.
#include "contracts/Editors.h"
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Halves.h"
#include "shared/Labels.h"
#include "shared/Presented.h"
#include "Kept.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
// A stack's param, the half its switch shows, as the switch's shown param sets it.
constexpr std::string_view shown_switch = "switch.shown";
// The recipes a move adds: a stack for a panel holding two contents, a panel for one torn
// out of a stack, and a dock to seat it.
constexpr std::string_view stack_recipe = "stack";
constexpr std::string_view panel_recipe = "panel";
constexpr std::string_view dock_recipe = "dock";
// An editor a file goes into, and the ports its text nodes chain by, the editors before
// one and those it hands on, as the text part names them.
constexpr std::string_view editor_recipe = "text-area";
constexpr std::string_view before_port = "before";
constexpr std::string_view editors_port = "editors";
// The keys part's, whose focus names the node the panel framed holds.
constexpr std::string_view typed_port = "typed";
// A deck a view goes into, as one of a panel holding several leaves for a panel of its
// own, named so its panel's button reads as the Perform panel's do.
constexpr std::string_view deck_name = "perform";
constexpr float even = 0.5f;      // a moved dock's ratio: half its place for each panel
constexpr float drag_from = 8.0f; // pixels the pointer goes from a press before it drags
constexpr std::int32_t ring = 2;  // how wide the frame where a drop puts a panel is
constexpr std::uint32_t frame_rects = 4;
// The frame round the panel holding the node in focus: two pixels wide, inside the panel,
// so it never covers the seams between panels, in the text's color faded halfway toward
// the ground, a light grey, so it marks the panel without drawing the eye from what the
// panel shows, nor reading as a thing selected, which the accent marks.
constexpr std::int32_t focus_ring = 2;
constexpr float focus_fade = 0.5f;
// A divider, then the focus's frame, then a drop's.
constexpr std::uint32_t rect_room = 1 + 2 * frame_rects;
// A panel's close mark, in the cells at the end of its tab strip that its tabs leave
// free, as the panel component's list param end does, which a press hides the panel by.
constexpr std::uint32_t close_cells = 2;
constexpr std::string_view close_text = "x";
constexpr std::uint32_t close_room = 32; // panels seated whose marks show, at most

// A panel a dock seats, where it shows, and its tab strip, which a drag takes it by, and
// where the view lists its bar, which orders the buttons.
struct Seated {
  std::string panel;
  VP_VIEW::Rect area;
  VP_VIEW::Rect strip;
  std::size_t listed = 0;
};

// The bar the docking part's area is the rest of, as the menubar's, when a seam parts its
// strip from that rest: where the seam is, which a drag hides the bar by and shows it
// again, the bar's size now, in rows, and the size it shows at.
struct Divider {
  std::string bar;
  VP_VIEW::Rect seam;
  std::uint32_t size = 0;
};

// A dock's split as the docks lay it out: where its area is, its axis and its ratio, and
// so its seam, which a drag moves.
struct Seam {
  std::string split;
  VP_VIEW::Rect area;
  glm::length_t along = 0;
  float ratio = 0.0f;
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

// The panel whose room a node takes, through what hands the room on, as a find bar's rest
// and a stack's switch, and each stack it is in with the half holding it, innermost
// first; none for a node no panel's room reaches. A node a panel holds follows it through
// moves, as its panel may go.
struct Holder {
  std::string panel;
  std::vector<std::pair<std::string, std::size_t>> halves;
};

std::optional<Holder> holder_of(const VP::View &view, std::string_view node) {
  Holder holder;
  for (std::string at(node);;) {
    const VP::Connection *const into = feeding(view, at);
    if (!into)
      return std::nullopt;
    const std::string &from = into->from.node;
    if (from.ends_with(bar_node) && into->from.port == rest_port) {
      holder.panel = from.substr(0, from.size() - bar_node.size());
      return holder;
    }
    const std::string stack = from.substr(0, from.size() - switch_node.size());
    const auto found = std::ranges::find(view.nodes, stack, &VP::Node::name);
    if (from.ends_with(switch_node) && found != view.nodes.end() &&
        found->recipe == stack_recipe)
      holder.halves.emplace_back(stack, into->from.port == half_ports[0] ? 0 : 1);
    at = from;
  }
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
// or the middle well inside it; one holding several, dropped on its own edge, splits, the
// one it shows going into a panel of its own there; of a deck, the view it shows goes
// into a deck of its own, or into the deck in the target's middle, so a pane and what it
// holds stay; let go on its own strip, a panel does nothing, as a tab let go on another
// moves among them there. An editor or a deck it added goes with its panel once it holds
// nothing, as its last tab closes or leaves. It hides a panel and shows it again,
// as its dock's ratio gives its half no room and back, and hands on a button a panel,
// lit while it shows, whose press hides or shows it. It finds where each
// panel and its strip show as the splits and the bars lay them out, from the Rect its
// area gives, which its host also hands the outermost dock, a seam being dragged too, as
// its split follows it, and while a drag goes on it frames where a drop would put the
// panel dragged. Of an editor holding several files,
// as the editors its host connects say, the file it shows moves: into a new editor, or
// into the editor in the target's middle. It keeps the panels as a person left them in
// .vulpen/layout.log beside the view's manifest, saved as they change, as the edits that
// make the manifest's panels these, and as the view
// starts as its manifest has it, arranges them so again, by panel restore, so they
// last between runs. A double click on a panel's strip, as on one of its tabs, gives it
// the docks' whole room, the others none, by panel maximize, and another gives them their
// room back. At the end of each strip it draws a close
// mark, whose press hides the panel, as its button does. Where the bar its area is the
// rest of parts its strip from it by a seam, as the menubar's, it draws that seam as a
// divider: a drag on it up past the middle of the strip hides the bar, its size 0, and
// down past it shows it again, at the size it had, the strip's size never changing.
class Docking final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string name = node.name();
    const auto connected = [&](std::string_view port) {
      return std::ranges::any_of(
          node.view().connections, [&](const VP::Connection &connection) {
            return std::ranges::any_of(connection.to, [&](const VP::Endpoint &to) {
              return to.node == name && to.port == port;
            });
          });
    };
    _editors =
        connected(editors_port) ? &node.input<VP_VIEW::Editors>(editors_port) : nullptr;
    _typed = connected(typed_port) ? &node.input<VP_VIEW::Typed>(typed_port) : nullptr;
    _move = node.command("panel move <node> <node> <value>",
                         "moves a panel beside another, at its left, right, top or "
                         "bottom, or into it, middle, which then holds both; of a panel "
                         "holding several, the one it shows, beside itself too, which "
                         "splits it in two; of a deck, the view it shows, which keeps "
                         "its pane");
    _hide = node.command("panel hide <node>",
                         "gives a panel no room, its dock's ratio leaving its half none, "
                         "or the dock's own when nothing beside it shows");
    _show = node.command("panel show <node>",
                         "gives a panel hidden its room again, as its docks' ratios were "
                         "before it hid, or halved; given a node a panel holds, as "
                         "ide.perform, the panel holding it now, its tab in front");
    _restore = node.command("panel restore",
                            "arranges the panels as a person left them, as .vulpen/"
                            "layout.log beside the view's manifest keeps them");
    _maximize = node.command("panel maximize <node>",
                             "gives a panel the docks' whole room, the others none, or, "
                             "given it already, the others the room they had");
    _manifest = node.view().file.string();
    _manifest_file = node.file(_manifest);
    _kept = (node.view().file.parent_path() / kept_folder / kept_file).string();
    _buttons = &node.output<VP_VIEW::Items>("buttons");
    _name = node.name();
    _view = node.view_name();
    _area = &node.input<VP_VIEW::Rect>("area");
    _font = &node.input<VP_VIEW::Font>("font");
    _rects = node.upload<VP_VIEW::Rect>("rects", rect_room);
    _labels = node.upload<VP_VIEW::Label>("labels", close_room);
    _characters = node.upload<VP_VIEW::Character>("characters", close_room);
    _laid_out.reset(); // a rebuild may have moved every panel
    _swept = false;    // and changed what a deck holds
    _slid.reset();     // its ratio set, as the drag let go of it sent it
    _framed_for.reset();
  }

  void cook(VP::Cook &frame) override {
    const glm::uvec2 cell = _font->cell;
    if (!_started)
      start(frame);
    if (const Laid laid{_area->offset, _area->extent, cell}; _laid_out != laid) {
      _laid_out = laid;
      seat_all(frame.view(), cell);
      find_divider(frame.view());
      keep(frame);
    }
    for (const VP::Event &event : frame.input().events())
      follow(frame, event, cell);
    sweep(frame);
    const std::span<VP_VIEW::Rect> rects = frame.write(_rects);
    std::uint32_t written = 0;
    if (_divider && _divider->seam.extent.x != 0)
      rects[written++] = divider_at(cell);
    if (const std::optional<std::size_t> framed = focused(frame.view()))
      written += ring_round(rects.subspan(written),
                            _seated[*framed].area,
                            focus_ring,
                            {.role = VP_VIEW::role("text"),
                             .blend_role = VP_VIEW::role("background"),
                             .blend = focus_fade});
    if (const auto drop = dropped())
      written += ring_round(rects.subspan(written),
                            half_at(_seated[drop->first].area, drop->second),
                            ring,
                            {.role = VP_VIEW::role("accent")});
    frame.write(_rects, written);
    write_marks(frame, cell);
  }

  // A panel whose content the docking part added, an editor or a deck, goes once that
  // content held a tab and holds none, as its last file closed or its last view closed or
  // left, by panel hide, which takes it: the last tab closed closes the panel. An
  // editor's files are read each frame, without allocating; a deck's views change only
  // with an edit, so they are read after a rebuild.
  void sweep(VP::Cook &frame) {
    if (!_written_read)
      return;
    if (_editors)
      for (const VP_VIEW::Editor *editor : *_editors) {
        const std::size_t count = editor->count();
        const auto had = _counts.find(editor->name());
        const bool emptied = editor != _editors->back() && count == 0 &&
                             had != _counts.end() && had->second > 0;
        if (had == _counts.end() || had->second != count)
          _counts[editor->name()] = count;
        if (emptied)
          hide_holding(frame, editor->name());
      }
    if (!std::exchange(_swept, true)) {
      std::vector<std::string> emptied;
      for (const VP::Node &node : frame.view().nodes) {
        if (node.recipe != deck_recipe || node_in(_written, node.name))
          continue;
        const bool holding = !held(frame.view(), *frame.view(""), node.name).empty();
        if (!holding && _decks_held[node.name])
          emptied.push_back(node.name);
        _decks_held[node.name] = holding;
      }
      for (const std::string &deck : emptied)
        hide_holding(frame, deck);
    }
  }

  // panel hide of the panel whose content holds the node.
  void hide_holding(VP::Cook &frame, std::string_view node) {
    const auto holding = std::ranges::find_if(_seated, [&](const Seated &seated) {
      return holds(frame.view(), seated.panel, node);
    });
    if (holding != _seated.end())
      frame.commands().send(std::format("{}: panel hide {}", _view, holding->panel));
  }

  // At the first frame, on a view as its manifest has it, the panels as the file keeps
  // them, through panel restore, so a line arranges them (V06); a view a replay of the log
  // arranged already, as a restart's, is left as it is, and so is one whose file was kept
  // for the manifest before it changed, which the next save keeps anew. With no file,
  // nothing is saved until the panels change.
  void start(VP::Cook &frame) {
    _started = true;
    VP::FilePort &files = frame.files();
    const bool read = read_manifest(frame);
    const std::filesystem::path kept(_kept);
    const std::vector<std::string> names = files.list(kept.parent_path().string());
    if (std::ranges::find(names, kept.filename().string()) == names.end()) {
      _saved = kept_text(_written, *frame.view(_view));
      return;
    }
    _saved = files.read(_kept);
    if (!read || !as_written(frame.view(), _written))
      return;
    if (kept_for(_saved) == fingerprint_of(_written))
      frame.commands().send(std::format("{}: panel restore", _view));
    else
      frame.log(VP::Level::warn, stale());
  }

  // Why a file is not replayed: its lines name what the manifest's nodes and connections
  // were, which have changed since, so they would join what the names now mean.
  std::string stale() const {
    return std::format("{} was kept for {} before its nodes or connections changed, so the "
                       "panels show as the manifest has them, and the next change keeps "
                       "them so",
                       _kept,
                       _manifest);
  }

  // The view as its manifest writes it, which the file keeps the panels against, read
  // again once the manifest changed, as view save writes it. Whether it could be read; a
  // manifest with a mistake is named, and the file kept as it is until it reads.
  bool read_manifest(VP::Cook &frame) {
    const std::string_view text = frame.files().text(_manifest_file);
    if (_written_read && text == _written_text)
      return true;
    try {
      _written = frame.files().manifest(_manifest);
      _written_text = text;
      _written_read = true;
    } catch (const std::runtime_error &mistake) {
      frame.log(VP::Level::warn, mistake.what());
      _written_read = false;
    }
    return _written_read;
  }

  // The panels as the view holds them now, saved once they changed: a panel moved, hidden
  // or shown, a seam or the menubar's divider dragged, a stack's other half shown, written
  // as the edits that make the manifest's panels these, so the file stays as short as
  // the panels' changes, however many edits made them.
  void keep(VP::Cook &frame) {
    if (!read_manifest(frame))
      return;
    std::string text = kept_text(_written, *frame.view(_view));
    if (text == _saved)
      return;
    frame.files().save(_kept, text);
    _saved = std::move(text);
  }

  // The kept file's lines on this view, as a script's run; the first a change refuses
  // stops it, the lines before staying, as a file kept before the manifest changed past
  // it may hold one, and the next change saves them as they are. Then every editor it
  // brought back that the docking part added goes, so a run starts with the manifest's
  // editors alone, and the docks go in the order they nest, as a file kept before moves
  // ordered them may list them otherwise. Its lines change the panels as the manifest
  // has them, so panels the file keeps as they are need none, and panels moved otherwise
  // are refused before a line changes them. The panels are as the manifest has them when
  // no edit makes them so, as the file is written: the view as edited holds a param the
  // manifest sets on a node inside a panel's on the panel's own, so it never reads the
  // same as the manifest's view, which the file port hands unfolded.
  void restore(VP::Call &call) {
    const std::filesystem::path kept(_kept);
    const std::vector<std::string> names = call.files().list(kept.parent_path().string());
    if (std::ranges::find(names, kept.filename().string()) == names.end())
      throw std::runtime_error(
          std::format("no panels are kept yet: {} keeps them once they change", _kept));
    const std::string text = call.files().read(_kept);
    if (!_written_read || kept_for(text) != fingerprint_of(_written))
      throw std::runtime_error(stale());
    const VP::View &now = *call.view(_view);
    if (kept_text(_written, now) == text)
      return call.reply(std::format("the panels are as {} keeps them already", _kept));
    if (!edits_from(_written, now).empty())
      throw std::runtime_error(std::format("{} changes the panels as {} has them, as a "
                                           "run starts, and they moved since",
                                           _kept,
                                           _manifest));
    const std::vector<std::string> lines = kept_lines(text);
    std::optional<std::string> stopped;
    for (const std::string &line : lines) {
      try {
        call.commands().send(std::format("{}: {}", _view, line));
      } catch (const std::runtime_error &refused) {
        stopped = std::format(
            "{}: {}: {}; the panels stay as restored so far", _kept, line, refused.what());
        break;
      }
    }
    if (_editors && !_editors->empty()) {
      const std::string &head = _editors->back()->name();
      const std::string_view own = parent_of(head);
      std::vector<std::string> added;
      for (const VP::Node &node : call.view(_view)->nodes)
        if (node.recipe == editor_recipe && node.name != own)
          added.push_back(node.name + head.substr(own.size()));
      for (const std::string &name : added)
        remove_editor(call, name);
    }
    std::vector<std::string> decks;
    for (const VP::Node &node : call.view(_view)->nodes)
      if (node.recipe == deck_recipe && !node_in(_written, node.name))
        decks.push_back(node.name);
    for (const std::string &deck : decks)
      remove_deck(call, deck);
    order_docks(call);
    if (stopped)
      throw std::runtime_error(*stopped);
  }

  // The bar its area is the rest of, when that bar parts its strip from the rest by a
  // seam: the seam, just above the area, its size, and the size it showed at last; from
  // the view as it runs, the bar's own node holding its params.
  void find_divider(const VP::View &view) {
    _divider.reset();
    const VP::Connection *const root = feeding(view, _name);
    if (!root || root->from.port != rest_port || !root->from.node.ends_with(bar_node))
      return;
    const auto node = std::ranges::find(view.nodes, root->from.node, &VP::Node::name);
    if (node == view.nodes.end() || param_of(*node, "seam") != "yes")
      return;
    const std::string_view size = param_of(*node, "size");
    _divider = Divider{
        .bar = root->from.node,
        .seam = {.offset = _area->offset - glm::ivec2(0, seam_width),
                 .extent = {_area->extent.x,
                            _area->extent.x == 0
                                ? 0u
                                : static_cast<std::uint32_t>(seam_width)}},
        .size = size.empty() ? 1u : number<std::uint32_t>(size)};
    if (_divider->size != 0)
      _shown_size = _divider->size;
  }

  // The divider, in the border's role, or the accent's under the pointer and while it is
  // dragged, which it follows, between the bar's top and where the strip ends shown.
  VP_VIEW::Rect divider_at(glm::uvec2 cell) const {
    VP_VIEW::Rect seam = _divider->seam;
    if (_dividing) {
      const std::int32_t top = seam.offset.y - strip_length(_divider->size, cell);
      seam.offset.y = std::clamp(static_cast<std::int32_t>(_pointer.y) - seam_width / 2,
                                 top,
                                 top + strip_length(_shown_size, cell));
    }
    seam.role = _dividing || VP_VIEW::contains(seam, _pointer) ? VP_VIEW::role("accent")
                                                                : VP_VIEW::role("border");
    return seam;
  }

  // How long a bar's strip of so many rows is, as the bar lays it out.
  static std::int32_t strip_length(std::uint32_t rows, glm::uvec2 cell) {
    return static_cast<std::int32_t>(bar_length(rows, cell.y, ~0u));
  }

  // Let go above the middle of the strip shown, the bar hides; below it, it shows.
  void let_go_divider(VP::Cook &frame, glm::uvec2 cell) {
    _dividing = false;
    const std::int32_t top = _divider->seam.offset.y - strip_length(_divider->size, cell);
    const std::uint32_t wanted =
        _pointer.y < static_cast<float>(top + strip_length(_shown_size, cell) / 2)
            ? 0
            : _shown_size;
    if (wanted != _divider->size)
      frame.commands().send(
          std::format("{}: param set {} size {}", _view, _divider->bar, wanted));
  }

  // Where a panel's close mark is: the cells at the end of its strip; none while the
  // strip has no room.
  static VP_VIEW::Rect close_of(const Seated &seated, glm::uvec2 cell) {
    const std::uint32_t wide = close_cells * cell.x;
    const VP_VIEW::Rect &strip = seated.strip;
    if (strip.extent.y == 0 || strip.extent.x < wide)
      return {};
    return {.offset = strip.offset +
                      glm::ivec2(static_cast<std::int32_t>(strip.extent.x - wide), 0),
            .extent = {wide, strip.extent.y}};
  }

  // An x a panel shown, in the middle of its close mark's cells, muted, or in the text's
  // color under the pointer.
  void write_marks(VP::Cook &frame, glm::uvec2 cell) {
    Labels written{frame.write(_labels), frame.write(_characters)};
    for (const Seated &seated : _seated) {
      const VP_VIEW::Rect mark = close_of(seated, cell);
      if (mark.extent.x == 0)
        continue;
      written.add({.offset = mark.offset + glm::ivec2((mark.extent - cell) / 2u),
                   .extent = cell,
                   .role = VP_VIEW::contains(mark, _pointer) ? VP_VIEW::role("text")
                                                             : VP_VIEW::role("muted")},
                  close_text);
    }
    frame.write(_labels, written.label_count);
    frame.write(_characters, written.character_count); // the used length glyphs draws
  }

  // From the dock its area's connection hands the same Rect, as far down as docks go;
  // then the buttons, in the order the view lists the panels, as they changed.
  void seat_all(const VP::View &view, glm::uvec2 cell) {
    _seated.clear();
    _seams.clear();
    _framed_for.reset();
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
      const float ratio = _slid && _slid->split == end.node
                              ? _slid->ratio
                              : number<float>(param_of(*node, "ratio"));
      _seams.push_back({.split = end.node, .area = area, .along = along, .ratio = ratio});
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
  // on another panel moves it there; a press on its close mark hides it. A seam pressed,
  // which its split's drag moves, moves the panels beside it as it goes, their strips and
  // marks with them, as the split works out its ratio, which it sets as it is let go.
  void follow(VP::Cook &frame, const VP::Event &event, glm::uvec2 cell) {
    if (event.kind == VP::Event::Kind::pointer) {
      _pointer = event.at;
      _dragging = _dragging || (_grabbed && glm::distance(_pointer, _from) > drag_from);
      if (_slid && _slid->held) {
        _slid->ratio =
            ratio_at(_slid->area, _slid->along, _slid->ratio, _pointer[_slid->along]);
        seat_all(frame.view(), cell);
      }
    } else if (event.kind == VP::Event::Kind::button && event.name == "left") {
      const auto closing = std::ranges::find_if(_seated, [&](const Seated &seated) {
        return VP_VIEW::contains(close_of(seated, cell), _pointer);
      });
      const auto seam = std::ranges::find_if(_seams, [&](const Seam &seam) {
        return VP_VIEW::contains(halves(seam.area, seam.along, seam.ratio).seam, _pointer);
      });
      if (event.down && _divider && VP_VIEW::contains(_divider->seam, _pointer)) {
        _dividing = true;
        _grabbed.reset();
      } else if (!event.down && _dividing) {
        let_go_divider(frame, cell);
      } else if (event.down && closing != _seated.end()) {
        frame.commands().send(std::format("{}: panel hide {}", _view, closing->panel));
        _grabbed.reset();
      } else if (event.down && seam != _seams.end()) {
        _slid = Slid{*seam};
        _grabbed.reset();
      } else if (!event.down && _slid && _slid->held) {
        _slid->held = false;
      } else if (event.down) {
        const auto pressed = std::ranges::find_if(_seated, [&](const Seated &seated) {
          return VP_VIEW::contains(seated.strip, _pointer);
        });
        _grabbed = pressed == _seated.end()
                       ? std::nullopt
                       : std::optional(std::size_t(pressed - _seated.begin()));
        _splits = _grabbed && several(frame.view(), *frame.view(""), pressed->panel);
        _from = _pointer;
        if (pressed != _seated.end() && event.presses == 2)
          frame.commands().send(
              std::format("{}: panel maximize {}", _view, pressed->panel));
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

  // The panel a drag would drop the one taken on, by the edge the pointer is nearest;
  // onto itself only a panel holding several, at an edge, which the one it shows leaves,
  // but never on its own strip, where a tab let go on another moves among them.
  std::optional<std::pair<std::size_t, std::string_view>> dropped() const {
    if (!_dragging || !_grabbed)
      return std::nullopt;
    for (std::size_t at = 0; at < _seated.size(); ++at) {
      const VP_VIEW::Rect &area = _seated[at].area;
      if (!VP_VIEW::contains(area, _pointer))
        continue;
      const glm::vec2 in = (_pointer - glm::vec2(area.offset)) / glm::vec2(area.extent);
      const std::array<float, 4> near{in.x, 1.0f - in.x, in.y, 1.0f - in.y};
      const auto nearest = std::ranges::min_element(near);
      const std::string_view side =
          *nearest > middle_from ? middle : sides[nearest - near.begin()];
      if (at == *_grabbed && (!_splits || side == middle ||
                              VP_VIEW::contains(_seated[at].strip, _pointer)))
        return std::nullopt;
      return std::pair(at, side);
    }
    return std::nullopt;
  }

  // Whether a panel holds several contents, as a stack, or an editor holding several
  // files, or a deck holding several views hosted, so the one it shows may leave it for a
  // panel beside it; a deck holding fewer in a stack still leaves the stack several.
  bool several(const VP::View &view,
               const VP::View &top,
               const std::string &panel) const {
    if (const std::string deck = deck_in(view, panel);
        !deck.empty() && held(view, top, deck).size() >= 2)
      return true;
    const std::optional<Content> held = content_of(view, panel);
    if (!held)
      return false;
    if (const VP_VIEW::Editor *const editor = editor_of(*held))
      return editor->count() > 1;
    return !stack_of(view, *held).empty();
  }

  // The deck a panel's content reaches, as the Rect its bar leaves reaches it, as the
  // Perform panel's, behind its switch, or one alone in a panel; empty for none.
  static std::string deck_in(const VP::View &view, std::string_view panel) {
    for (const VP::Node &node : view.nodes)
      if (node.recipe == deck_recipe && holds(view, panel, node.name))
        return node.name;
    return {};
  }

  // The views a deck holds that are hosted beside the view it runs in, in its order.
  std::vector<std::string> held(const VP::View &view,
                                const VP::View &top,
                                std::string_view deck) const {
    const auto node = std::ranges::find(view.nodes, deck, &VP::Node::name);
    std::vector<std::string> views =
        node == view.nodes.end() ? std::vector<std::string>()
                                 : split_names(param_value(*node, holds_param));
    auto hosted = projects(top, _view);
    std::erase_if(views, [&](const std::string &name) {
      return std::ranges::none_of(
          hosted, [&](const VP::Child &child) { return child.name == name; });
    });
    return views;
  }

  // A frame round a Rect, width pixels wide, inside it, in the colors given. Returns how
  // many Rects it took.
  static std::uint32_t ring_round(std::span<VP_VIEW::Rect> rects,
                                  const VP_VIEW::Rect &at,
                                  std::int32_t width,
                                  const VP_VIEW::Rect &colors) {
    const glm::ivec2 low = at.offset;
    const glm::uvec2 size = at.extent;
    const auto wide = static_cast<std::uint32_t>(width);
    rects[0] = {.offset = low, .extent = {size.x, wide}};
    rects[1] = {.offset = low + glm::ivec2(0, static_cast<std::int32_t>(size.y) - width),
                .extent = {size.x, wide}};
    rects[2] = {.offset = low, .extent = {wide, size.y}};
    rects[3] = {.offset = low + glm::ivec2(static_cast<std::int32_t>(size.x) - width, 0),
                .extent = {wide, size.y}};
    for (VP_VIEW::Rect &edge : rects.first(frame_rects)) {
      edge.role = colors.role;
      edge.blend_role = colors.blend_role;
      edge.blend = colors.blend;
    }
    return frame_rects;
  }

  // The panel shown whose content holds the node in focus, found again only as the focus
  // moves or the panels are seated anew, so a frame allocates nothing for it; none while
  // no node has the focus, or none of its host's typed reaches it.
  std::optional<std::size_t> focused(const VP::View &view) {
    if (!_typed || (_framed_for && *_framed_for == _typed->focus))
      return _framed;
    _framed_for = _typed->focus;
    _framed.reset();
    for (std::size_t at = 0; at < _seated.size() && !_typed->focus.empty(); ++at)
      if (_seated[at].area.extent.x != 0 && _seated[at].area.extent.y != 0 &&
          holds(view, _seated[at].panel, _typed->focus))
        _framed = at;
    return _framed;
  }

  // Whether a panel's content holds the node, as the Rect its bar leaves reaches it: from
  // the bar, along each connection into a node's area, as a find bar hands its text the
  // rest of its own and a switch hands one half the room.
  static bool holds(const VP::View &view, std::string_view panel, std::string_view node) {
    std::vector<std::string> reached{std::string(panel) + std::string(bar_node)};
    for (std::size_t at = 0; at < reached.size(); ++at)
      for (const VP::Connection &connection : view.connections)
        if (connection.from.node == reached[at])
          for (const VP::Endpoint &to : connection.to) {
            if (to.port != area_port ||
                std::ranges::find(reached, to.node) != reached.end())
              continue;
            if (to.node == node)
              return true;
            reached.push_back(to.node);
          }
    return false;
  }

  void command(VP::Call &call) override {
    if (call.is(_move))
      move(call);
    else if (call.is(_hide))
      hide(call, std::string(call.arguments().front()));
    else if (call.is(_show))
      show(call, std::string(call.arguments().front()));
    else if (call.is(_restore))
      restore(call);
    else if (call.is(_maximize))
      maximize(call, std::string(call.arguments().front()));
  }

  // Every dock above the panel gives its half all of its room, each ratio kept as hide
  // keeps it, so panel show of another gives its room back; a panel given all of it
  // already gets back the ratios its maximize changed, or after a restart, which forgets
  // them, every dock above it its ratio from before it hid a half, or halved.
  void maximize(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    std::vector<Seating> way; // from the panel up, read before a line changes the view
    for (std::optional<Seating> seating = seating_of(view, panel + std::string(bar_node));
         seating;
         seating = seating_of(view, seating->split))
      way.push_back(*seating);
    if (way.empty())
      throw std::runtime_error(std::format("{} is no panel a dock seats", panel));
    const auto all = [](const Seating &seating) {
      return seating.gives_none(1 - seating.half);
    };
    if (std::ranges::all_of(way, all)) {
      for (const Seating &seating : way) {
        if (_maximized.panel == panel &&
            std::ranges::find(_maximized.splits, seating.split) == _maximized.splits.end())
          continue;
        const auto kept = _ratios.find(seating.split);
        send(call,
             std::format("param set {} ratio {}",
                         seating.split,
                         kept != _ratios.end() && kept->second > 0.0f && kept->second < 1.0f
                             ? kept->second
                             : even));
      }
      _maximized = {};
      return;
    }
    _maximized = {.panel = panel};
    for (const Seating &seating : way) {
      if (all(seating))
        continue;
      if (!seating.gives_none(seating.half))
        _ratios[seating.split] = seating.ratio;
      _maximized.splits.push_back(seating.split);
      send(call,
           std::format("param set {} ratio {}", seating.split, seating.half == 0 ? 1 : 0));
    }
  }

  // The dock seating the panel gives its half no room, or, when its other half shows
  // nothing either, the dock's own dock gives the dock none, and so up, so nothing
  // hidden keeps a seam. A panel holding an editor the docking part added, which holds
  // no file, goes with it instead, and so does its button.
  void hide(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    if (const std::string deck = deck_in(view, panel);
        !deck.empty() && _written_read && !node_in(_written, deck) &&
        held(view, *call.view(""), deck).empty()) {
      remove_deck(call, deck);
      return;
    }
    if (const std::optional<Content> held = content_of(view, panel)) {
      VP_VIEW::Editor *const editor = editor_of(*held);
      if (editor && editor != _editors->back() && editor->count() == 0) {
        remove_editor(call, editor->name());
        return;
      }
    }
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
  // of it, as what else it holds was hidden before. A node a panel holds shows that
  // panel, each stack it is in showing the half holding it, so a part naming what it
  // shows in finds it after a move took it into another panel.
  void show(VP::Call &call, const std::string &panel) {
    const VP::View &view = *call.view(_view);
    std::vector<Seating> way; // from the panel up, read before a line changes the view
    for (std::optional<Seating> seating = seating_of(view, panel + std::string(bar_node));
         seating;
         seating = seating_of(view, seating->split))
      way.push_back(*seating);
    if (const std::optional<Holder> holder = way.empty() ? holder_of(view, panel)
                                                         : std::nullopt) {
      std::vector<std::string> fronts; // read before a line changes the view
      for (const auto &[stack, half] : holder->halves)
        if (param_of(*std::ranges::find(view.nodes, stack, &VP::Node::name),
                     shown_switch) != half_ports[half])
          fronts.push_back(std::format("param set {}{} {} {}",
                                       stack,
                                       switch_node,
                                       shown_param,
                                       half_ports[half]));
      for (const std::string &line : fronts)
        send(call, line);
      return show(call, holder->panel);
    }
    if (way.empty())
      throw std::runtime_error(
          std::format("{} is no panel a dock seats, nor a node one holds", panel));
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
    const VP::View &top = *call.view("");
    // Before a deck's view moves too, which seats a panel in the target's place.
    if (!feeding(view, target + std::string(bar_node)) || !content_of(view, target))
      throw std::runtime_error(std::format(
          "{} moves beside or into another panel a dock seats, not {}; ls lists the "
          "nodes",
          panel,
          target));
    if (const std::string deck = deck_in(view, panel); !deck.empty()) {
      const bool into_deck =
          panel != target && side == middle && !deck_in(view, target).empty();
      if (into_deck || (side != middle && held(view, top, deck).size() >= 2)) {
        move_view(call, deck, panel, target, side);
        order_docks(call);
        return;
      }
    }
    const VP::Connection *const into = feeding(view, panel + std::string(bar_node));
    const std::optional<Content> held = content_of(view, panel);
    if (panel == target && (side == middle || !several(view, top, panel)))
      throw std::runtime_error(std::format(
          "{} splits beside itself only holding several, at an edge, the one it shows "
          "leaving it",
          panel));
    if (!into || !into->from.node.ends_with(split_node) || !held)
      throw std::runtime_error(
          std::format("{} is no panel a dock seats; ls lists the nodes", panel));
    if (VP_VIEW::Editor *const editor = editor_of(*held); editor && editor->count() > 1) {
      move_file(call, *editor, panel, target, side);
    } else if (!stack_of(view, *held).empty()) {
      const Content going = unstack(call, panel);
      if (side == middle)
        join(call, going, target);
      else
        seat_new(call, going, panel, target, side);
    } else if (side == middle) {
      const Content going = *held;
      leave(call, panel);
      send(call, std::format("node remove keep {}", panel));
      join(call, going, target);
    } else {
      move_beside(call, panel, target, side);
    }
    order_docks(call);
  }

  // Every dock listed before the panels, each after the dock seating it, from the
  // outermost down: a dock draws the ground over its whole area, which covers the seam of
  // a dock inside it drawn before it, as one a move put inside a dock listed after it.
  // As node moves, which the kept file names, so a restart lists them so too.
  void order_docks(VP::Call &call) {
    const VP::View &view = *call.view(_view);
    std::vector<std::string> docks;
    std::size_t first_panel = view.nodes.size();
    if (const VP::Connection *const root = feeding(view, _name))
      for (const VP::Endpoint &to : root->to)
        if (to.node != _name)
          nest(view, to, docks, first_panel);
    const auto listed = [&](std::string_view node) {
      return std::ranges::find(view.nodes, node, &VP::Node::name) - view.nodes.begin();
    };
    if (first_panel == view.nodes.size() || docks.empty() ||
        (std::ranges::is_sorted(docks, {}, listed) &&
         static_cast<std::size_t>(listed(docks.back())) < first_panel))
      return;
    const std::string before = view.nodes[first_panel].name; // each edit replaces the view
    for (const std::string &dock : docks)
      send(call, std::format("node move {} {}", dock, before));
  }

  // The docks a Rect reaches, a dock before those its halves seat, the first half's
  // before the second's, and where the view lists the first panel they seat.
  void nest(const VP::View &view,
            const VP::Endpoint &end,
            std::vector<std::string> &docks,
            std::size_t &first_panel) const {
    if (end.port != area_port)
      return;
    if (end.node.ends_with(split_node)) {
      docks.push_back(end.node.substr(0, end.node.size() - split_node.size()));
      for (const std::string_view half : half_ports)
        if (const VP::Connection *const seated = from(view, end.node, half))
          for (const VP::Endpoint &to : seated->to)
            nest(view, to, docks, first_panel);
    } else if (end.node.ends_with(bar_node)) {
      const std::string_view panel =
          std::string_view(end.node).substr(0, end.node.size() - bar_node.size());
      first_panel = std::min(
          first_panel,
          static_cast<std::size_t>(std::ranges::find(view.nodes, panel, &VP::Node::name) -
                                   view.nodes.begin()));
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
    send(call, std::format("node remove keep {}", parent_of(split)));
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
    const std::size_t shown = param_of(*node, shown_switch) == half_ports[1] ? 1 : 0;
    const std::optional<Content> going = half_of(view, stack, shown);
    const std::optional<Content> staying = half_of(view, stack, 1 - shown);
    if (!going || !staying)
      throw std::runtime_error(std::format("{} holds no content in a half", stack));
    const VP::Connection room = *from(view, panel + std::string(bar_node), rest_port);
    const VP::Connection tabs =
        *into(view, panel + std::string(tabs_node), items_port);
    send(call, std::format("node remove keep {}", stack));
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
         std::format("node add {} recipe={} param={}={}",
                     stack,
                     stack_recipe,
                     shown_switch,
                     half_ports[1]));
    // Listed before the target, the content joining it too, so the panel's strip draws
    // over both, and before what draws over the panels.
    send(call, std::format("node move {} {}", stack, target));
    send(call,
         std::format(
             "node move {}.{} {}", parent_of(target), content_name(content), target));
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

  // The view a deck shows goes into the deck in the target's middle, or into a deck of
  // its own, in a new panel beside the target, as a file an editor shows goes into
  // another editor: the decks' holds and shown params follow, and the connection bringing
  // the view's pane its room then comes from the deck holding it, so the pane, and what
  // it holds, as a frame held still, stay. A new deck takes what the deck the view left
  // takes, but its room, as the keys part's typed.
  void move_view(VP::Call &call,
                 const std::string &deck,
                 const std::string &panel,
                 const std::string &target,
                 std::string_view side) {
    const VP::View &view = *call.view(_view);
    const VP::View &top = *call.view("");
    const auto hosts = [&](std::string_view name) {
      return std::ranges::any_of(projects(top, _view), [&](const VP::Child &child) {
        return child.name == name;
      });
    };
    const auto node_of = [&](std::string_view name) {
      return std::ranges::find(view.nodes, name, &VP::Node::name);
    };
    // Copied first: each edit replaces the view these point into.
    const std::string moved = deck_shows(*node_of(deck), shown_by(view), hosts);
    if (moved.empty())
      throw std::runtime_error(std::format("{} shows no view to move", deck));
    std::vector<std::string> left = split_names(param_value(*node_of(deck), holds_param));
    std::erase(left, moved);
    std::string into = side == middle ? deck_in(view, target) : std::string();
    std::vector<std::string> taking =
        into.empty() ? std::vector<std::string>()
                     : split_names(param_value(*node_of(into), holds_param));
    const VP::Connection *const room = from(view, deck, moved);
    const std::optional<VP::Connection> pane = room ? std::optional(*room) : std::nullopt;
    if (into.empty()) {
      into = free_node(view, std::format("{}.{}", parent_of(target), deck_name));
      std::vector<VP::Connection> grown;
      for (const VP::Connection &connection : view.connections) {
        VP::Connection more = connection;
        for (const VP::Endpoint &to : connection.to)
          if (to.node == deck && to.port != area_port)
            more.to.push_back({into, to.port});
        if (more.to.size() != connection.to.size())
          grown.push_back(std::move(more));
      }
      send(call,
           std::format("node add {} recipe={} param={}={} param={}={}",
                       into,
                       deck_recipe,
                       holds_param,
                       moved,
                       shown_param,
                       moved));
      send(call, std::format("node move {} {}", into, _name));
      for (const VP::Connection &connection : grown) {
        send(call, std::format("disconnect {}", connection.name));
        connect(call, connection);
      }
      seat_new(call,
               {.area = {{into, std::string(area_port)}},
                .tabs = {into, std::string(tabs_port)}},
               panel,
               target,
               side);
    } else {
      taking.push_back(moved);
      send(call,
           std::format("param set {} {} {}", into, holds_param, joined_names(taking)));
      send(call, std::format("param set {} {} {}", into, shown_param, moved));
    }
    send(call,
         left.empty()
             ? std::format("param unset {} {}", deck, holds_param)
             : std::format("param set {} {} {}", deck, holds_param, joined_names(left)));
    if (pane) {
      send(call, std::format("disconnect {}", pane->name));
      connect(call, {.name = pane->name, .from = {into, moved}, .to = pane->to});
    }
  }

  // The editor whose tabs a panel's content hands on; none for none.
  VP_VIEW::Editor *editor_of(const Content &content) const {
    if (!_editors)
      return nullptr;
    const auto found = std::ranges::find_if(*_editors, [&](const VP_VIEW::Editor *editor) {
      return editor->name() == content.tabs.node;
    });
    return found == _editors->end() ? nullptr : *found;
  }

  // The file the editor shows goes into the editor in the target's middle, or into a new
  // one, in a panel of its own beside the target, or stacked in its middle; then it closes
  // in the editor it left, so a file is open in one editor at a time. A file with changes
  // stays, as closing it would drop them.
  void move_file(VP::Call &call,
                 VP_VIEW::Editor &editor,
                 const std::string &panel,
                 const std::string &target,
                 std::string_view side) {
    const std::string file(editor.shown());
    if (editor.changed())
      throw std::runtime_error(
          std::format("{} has changes: write saves them before it moves", file));
    const std::optional<Content> there = content_of(*call.view(_view), target);
    VP_VIEW::Editor *const taking = side == middle && there ? editor_of(*there) : nullptr;
    const std::string text =
        taking ? taking->name() : add_editor(call, editor, panel, target, side);
    send(call, std::format("param set {} file {}", text, file));
    send(call, std::format("close {}", file));
  }

  // A text-area like the editor's, listed before the docking part as a panel is, taking
  // what the editor's own takes but its room, its theme, its font, its atlas and its keys,
  // and its place in the chain of editors just before the last, which answers the
  // editor's commands; in a panel of its own beside the target, or stacked in its middle.
  // Returns its text node.
  std::string add_editor(VP::Call &call,
                         const VP_VIEW::Editor &editor,
                         const std::string &panel,
                         const std::string &target,
                         std::string_view side) {
    const VP::View &view = *call.view(_view);
    const std::string_view text_node = editor.name();
    const std::string content(parent_of(text_node));
    const std::string fresh = free_node(view, content);
    const std::string text = fresh + std::string(text_node.substr(content.size()));
    const std::string own = content + ".";
    std::vector<VP::Connection> grown;
    for (const VP::Connection &connection : view.connections) {
      VP::Connection more = connection;
      for (const VP::Endpoint &to : connection.to)
        if (to.node.starts_with(own) && to.port != area_port && to.port != before_port)
          more.to.push_back({fresh + to.node.substr(content.size()), to.port});
      if (more.to.size() != connection.to.size())
        grown.push_back(std::move(more));
    }
    const std::string last = _editors->back()->name();
    const VP::Connection *const chain = into(view, last, before_port);
    const std::optional<VP::Connection> chained =
        chain ? std::optional(*chain) : std::nullopt;
    send(call, std::format("node add {} recipe={}", fresh, editor_recipe));
    send(call, std::format("node move {} {}", fresh, _name));
    for (const VP::Connection &connection : grown) {
      send(call, std::format("disconnect {}", connection.name));
      connect(call, connection);
    }
    if (chained) {
      send(call, std::format("disconnect {}", chained->name));
      connect(call,
              {.name = free_name(call, std::format("{}-before", short_name(fresh))),
               .from = chained->from,
               .to = {{text, std::string(before_port)}}});
    }
    connect(call,
            {.name = free_name(call, std::format("{}-editors", short_name(fresh))),
             .from = {text, std::string(editors_port)},
             .to = {{last, std::string(before_port)}}});
    const Content content_of_fresh{.area = {{text, std::string(area_port)}},
                                   .tabs = {text, std::string(tabs_port)}};
    if (side == middle)
      join(call, content_of_fresh, target);
    else
      seat_new(call, content_of_fresh, panel, target, side);
    return text;
  }

  // An editor the docking part added goes, by its text node, its files with it: the panel
  // holding it alone goes too, the dock seating that panel giving its other half its
  // place, or from a stack the stack goes, leaving its panel the content beside it; and
  // the chain of editors closes over it, the editors before it reaching the one after.
  void remove_editor(VP::Call &call, std::string_view text) {
    const VP::View &view = *call.view(_view);
    const std::string editor(parent_of(text));
    // Copied first: each edit replaces the view these point into.
    const VP::Connection *const before = into(view, text, before_port);
    const VP::Connection *const after = from(view, text, editors_port);
    const std::optional<VP::Connection> chained =
        before && after ? std::optional(VP::Connection{
                              .name = before->name, .from = before->from, .to = after->to})
                        : std::nullopt;
    remove_seated(call, text);
    send(call, std::format("node remove keep {}", editor));
    if (chained)
      connect(call, *chained);
  }

  // A deck the docking part added goes, with the panel holding it, as an editor it added
  // does.
  void remove_deck(VP::Call &call, const std::string &deck) {
    remove_seated(call, deck);
    send(call, std::format("node remove keep {}", deck));
  }

  // The panel holding alone the content whose room the node takes goes, the dock seating
  // it giving its other half its place, or from a stack the stack goes, leaving its panel
  // the content beside it.
  void remove_seated(VP::Call &call, std::string_view node) {
    const VP::View &view = *call.view(_view);
    const VP::Connection *const area = feeding(view, node);
    const std::string room = area ? area->from.node : std::string();
    const std::string port = area ? area->from.port : std::string();
    if (room.ends_with(bar_node) && port == rest_port) {
      const std::string panel = room.substr(0, room.size() - bar_node.size());
      leave(call, panel);
      send(call, std::format("node remove keep {}", panel));
    } else if (room.ends_with(switch_node)) {
      const VP::Connection *const holding = feeding(view, room);
      if (!holding || !holding->from.node.ends_with(bar_node))
        throw std::runtime_error(std::format("{} is in no panel", room));
      const std::string panel =
          holding->from.node.substr(0, holding->from.node.size() - bar_node.size());
      send(call,
           std::format("param set {} shown {}", room, port == half_ports[1] ? half_ports[1] : half_ports[0]));
      unstack(call, panel);
    }
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

  // Each line on its own view, whichever view the line that ran the command addressed.
  void send(VP::Call &call, std::string_view line) {
    call.commands().send(std::format("{}: {}", _view, line));
  }

  VP::Command _move;
  VP::Command _hide;
  VP::Command _show;
  VP::Command _restore;
  VP::Command _maximize;
  // Where the view's manifest is, watched, and the view it writes, as read last, and
  // whether it read; the file keeping the panels beside it, as saved last, and whether
  // the first frame read it.
  std::string _manifest;
  VP::File _manifest_file;
  VP::View _written;
  std::string _written_text;
  bool _written_read = false;
  std::string _kept;
  std::string _saved;
  bool _started = false;
  VP_VIEW::Items *_buttons = nullptr;
  const VP_VIEW::Editors *_editors = nullptr; // as its host connects them; none for none
  const VP_VIEW::Typed *_typed = nullptr;     // so too
  // The panel framed, as the focus it was found for and the panels as seated then; none
  // found for yet after a rebuild, or since the panels were seated anew.
  std::optional<std::string> _framed_for;
  std::optional<std::size_t> _framed;
  // How many files each editor held, by its text node, and whether each deck the docking
  // part added held a view, as last seen, so one that held some and holds none goes.
  std::map<std::string, std::size_t, std::less<>> _counts;
  std::map<std::string, bool, std::less<>> _decks_held;
  bool _swept = false; // the decks, since the last rebuild
  // A dock's ratio before it hid a half, by its split, so showing it gives it back; and the
  // panel maximized last, with the splits its maximize gave the panel's half all of.
  std::map<std::string, float, std::less<>> _ratios;
  struct Maximized {
    std::string panel;
    std::vector<std::string> splits;
  };
  Maximized _maximized;
  std::string _name;
  std::string _view; // the one it runs in, whose docks it edits
  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels; // the close marks'
  VP::Upload<VP_VIEW::Character> _characters;
  std::vector<Seated> _seated; // as the docks lay them out now
  std::vector<Seam> _seams;    // and the seams between them
  // The seam a drag moves, and its ratio as the split works it out from the pointer, held
  // until let go and kept until the rebuild that sets it.
  struct Slid : Seam {
    bool held = true;
  };
  std::optional<Slid> _slid;
  // The area and the font's cell the panels were seated for; none until a frame seats
  // them, and after a rebuild.
  struct Laid {
    glm::ivec2 offset;
    glm::uvec2 extent;
    glm::uvec2 cell;
    bool operator==(const Laid &) const = default;
  };
  std::optional<Laid> _laid_out;
  std::optional<Divider> _divider; // the bar above the area, when a seam parts it from it
  std::uint32_t _shown_size = 1;   // the bar's size while it showed last, in rows
  bool _dividing = false;          // its divider was pressed, and is not let go yet
  glm::vec2 _pointer{};
  glm::vec2 _from{};                   // where the press that grabbed a panel was
  std::optional<std::size_t> _grabbed; // the panel whose strip was pressed
  bool _splits = false;                // it holds several, so a drop on itself splits it
  bool _dragging = false;              // it went far enough from the press to drag
};

} // namespace

VP_OPERATOR(Docking);
