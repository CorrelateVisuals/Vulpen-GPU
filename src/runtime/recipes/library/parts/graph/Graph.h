#pragma once

#include "contracts/Curve.h"
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Relation.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "shared/Labels.h"
#include "shared/PanZoom.h"
#include "Arranged.h"
#include "Boxes.h"
#include "Covered.h"
#include "Hover.h"
#include "Layers.h"
#include "Layout.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Only Graph.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the part in one binary never clash.
namespace {

// A box pressed: which, where the pointer was then, the boxes a drag moves, each with
// where it was then, and whether it has been dragged, or is a press let go where it was.
// Or a corner of a
// box pressed, which a drag pulls while the opposite corner stays: whether it is a left
// one and whether a top one, and the box's size and place then.
struct Grab {
  std::size_t box = 0;
  glm::vec2 from{};
  std::vector<std::pair<std::size_t, glm::ivec2>> moving;
  bool moved = false;
  std::optional<glm::bvec2> corner;
  glm::ivec2 extent{};
  glm::ivec2 offset{};
};

// The box under a point, and the corner of it the point is on, if any.
struct Under {
  std::size_t box = no_box;
  std::optional<glm::bvec2> corner;
};

// The graph of the current view, as the terminals' lines go to it, which view switch
// moves and a second press on a view's box moves into it, as the view runs, each recipe a
// node uses unfolded, a level at a time, as the POC showed it: the nodes at the top and
// the views the view hosts, or the inside of the node its inside param names, that node's
// box framing the boxes of the nodes inside it, and beside it what lies outside at either
// side, where connections reach in and out. A box a node, with its name, its ports, those
// its insides read and write across its edge too, the recipe it uses, the nodes inside it
// and its files; a box each view it hosts, with its name, its window and the file it is
// hosted from; and a curve a reader of a connection, from the writer's port on its box's
// right side to the reader's on its left, a view's from its window where its host takes
// it, at right angles in the lanes between the boxes, so it runs over none and no other's
// lane. A second press on a box holding nodes, or zooming in on it, shows its inside, and
// one on the frame, or zooming out, the level above, through the inside param, which a
// line sets as a press does (V06). The manifest holds no places (V04), so the graph alone
// lays it out: the boxes inside the frame, or those at the top, a column past every box
// beside them they read from, down each column in the manifest's order. A box dragged
// stays where it is put, the frame and what lies outside too, and the first drag places
// every box where it shows, so a change to the graph moves none; the boxes placed are a
// look at the graph, not its plumbing, so they go in .vulpen/graph.ini beside the view's
// manifest, and a restart shows them as they were left; so do the colors a person gives
// each kind of node there, which draw its boxes' borders and names. The graph starts at
// home, as a view or a level is shown and as h brings it there: unmoved at zoom 1 while
// every box shows whole there, else moved and zoomed out until they all do; but a level
// gone up to shows the box it was left from in focus, under the pointer, two steps of
// the wheel short of going into it again. . goes into the box selected, or the one under
// the pointer, and , up a level, as the wheel and a second press do. A drag on the
// ground, or on the frame's inside, which shows the ground, moves the graph and the wheel
// zooms it about the pointer, as the Perform panel's image moves, and neither is kept. A
// press on a box selects it, shift adding it or taking it out, and a second press opens
// the file of a row that names one in the editor; a drag on a box selected moves every
// box selected,
// and one on a box no line selects, as what lies outside, moves that box. A drag on a
// corner of a box makes it larger, or smaller down to the size its rows and the boxes
// inside it need, and its corners show while the pointer is over it. The box selected
// or pressed last shows over the others, hiding the boxes and the rows behind it. With
// shift, a drag on the ground draws a select box, and letting go adds every box it
// touches to the selection. The curve under the pointer, where no box lies, shows over
// the others, brighter, and its details, a connection's ends or the lines that say the
// relations it stands for, go as rows to a tooltip beside the pointer.
// It owns the selection, of nodes and of the views the view shown hosts, which select and
// selected reach as lines do (V06), and which delete or backspace removes as node remove
// and view close do, and takes keys from the keys part while it has the focus, which a
// press in its area gives it. Its own shaders draw the ground, lines that move and zoom
// with the graph over the whole area, as the POC's grid did; it writes Rects, Labels and
// Curves for the parts that draw over them, and only what lies in its area: a box cut at
// its edge, and a row of text with the area as its room, which glyphs cuts at the edge; a
// curve goes whole, and the curves part cuts it at the edge of the ground the graph hands
// it.
class Graph final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;
  void command(VP::Call &call) override;

private:
  void key(VP::Cook &frame, std::string_view key);
  void show(std::string_view name);
  void arrange(VP::Cook &frame, const std::filesystem::path &beside);
  void build(const VP::View *edited, const VP::View *flat);
  void name_tab();
  void show_view(VP::Cook &frame) const;
  void show_folder(VP::Cook &frame, const std::filesystem::path &folder) const;
  void home(glm::uvec2 cell);
  void follow(VP::Cook &frame, bool room, const VP::Event &event);
  void press(VP::Cook &frame, std::uint32_t presses);
  void go_in(VP::Cook &frame);
  void focus(std::string_view left);
  // A folder box's folder, as an absolute path.
  std::filesystem::path folder_of(std::size_t box) const;
  void grab_corner(VP::Cook &frame, const Under &under);
  void drag(glm::vec2 to);
  void pull(glm::vec2 to);
  void place_all();
  void release(VP::Cook &frame, bool room);
  bool open(VP::Cook &frame, std::size_t at);
  void turn(VP::Cook &frame, glm::vec2 turn);
  bool enter(VP::Cook &frame, std::size_t at);
  void leave(VP::Cook &frame);
  bool selected(std::string_view node) const;
  void select(VP::CommandPort &commands, std::span<const std::string> nodes) const;
  void select_touched(VP::CommandPort &commands, glm::vec2 from) const;
  void erase(VP::Cook &frame, const VP::View &view);
  void save(VP::Cook &frame);
  void keep(VP::FilePort &files);
  void write(VP::Cook &frame, glm::uvec2 cell);
  void write_box(std::span<VP_VIEW::Rect> rects, std::uint32_t &written, std::size_t at) const;
  void write_corners(std::span<VP_VIEW::Rect> rects, std::uint32_t &written) const;
  void write_select_box(std::span<VP_VIEW::Rect> rects, std::uint32_t &written) const;
  void write_labels(VP::Cook &frame, glm::uvec2 cell);
  void write_row(glm::vec2 at,
                 std::string_view row,
                 std::uint32_t role,
                 glm::uvec2 cell,
                 std::span<const VP_VIEW::Rect> over,
                 Labels &written) const;
  void write_curves(VP::Cook &frame);
  void write_tip();
  std::size_t more_under() const;
  std::size_t row_under(std::size_t at) const;
  // The curve nearest a point on screen, near enough, where no box lies; none while a
  // drag goes on.
  std::size_t curve_under(glm::vec2 point) const;
  float corner(std::span<const Waypoint> way, std::size_t at) const;
  // Where a place of the graph shows this frame, in pixels from the window's top left.
  glm::vec2 on_screen(glm::vec2 at) const;
  // The middle of a box's port row, at its left edge or its right, on screen.
  glm::vec2 port_of(std::size_t box, std::size_t row, bool right) const;
  // The box drawn last over a point on screen, or none, for the ground.
  std::size_t hit(glm::vec2 at) const;
  // That box, or one whose corner the point is on, as a frame's below its rows.
  Under under(glm::vec2 at) const;
  // Drawn over the others from now on, until another is.
  void raise(std::string_view name);
  std::optional<std::uint32_t> color_of(std::size_t box) const;
  VP_VIEW::Rect box(std::size_t at) const;
  // What of a box hides what lies under it: all of it, or of a frame its rows, as the
  // ground shows through below them.
  VP_VIEW::Rect solid(std::size_t at) const;
  VP_VIEW::Rect select_box(glm::vec2 from) const;

  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  // What links the files shown, and the folder shown in place of the view, if any.
  const VP_VIEW::Relations *_relations = nullptr;
  std::string _name; // as the focus names it
  std::string _view; // the one it runs in, where its inside param is set
  // The node whose inside the graph shows, in the view shown; with none, or one that view
  // does not hold, the top.
  std::string _inside;
  VP::Command _select;
  VP::Command _selected;
  VP::Upload<VP_VIEW::Rect> _ground; // its area, which its own shaders draw lines on
  VP::Value<glm::vec2> _pan;
  VP::Value<float> _zoom;
  VP::Upload<VP_VIEW::Rect> _rects;
  VP::Upload<VP_VIEW::Label> _labels;
  VP::Upload<VP_VIEW::Character> _characters;
  VP::Upload<VP_VIEW::Curve> _curves;
  VP_VIEW::Items *_tabs = nullptr;
  VP_VIEW::Items *_buttons = nullptr; // a layer each, along the graph's bottom edge
  // The details of the curve under the pointer, a row an Item, and where they show.
  VP_VIEW::Items *_tip = nullptr;
  VP_VIEW::Rect *_tip_place = nullptr;
  std::size_t _hovered_end = no_box;
  std::size_t _hovered_more = no_box; // a box whose row for the rest the pointer is over
  std::filesystem::path _base; // where the paths of the boxes built are named from
  Shown _layers = every_layer;        // as its relations param names them
  std::string _shown; // the view the tab names; empty for the one vulpen started with
  // The boxes of the view shown, built again only when the views they were built from
  // change, which each edit replaces, and laid out again only then, as the font's cell
  // changes, or as a box is dragged, so a frame allocates nothing for them.
  Boxes _boxes;
  const VP::View *_built_edited = nullptr;
  const VP::View *_built_flat = nullptr;
  // The views the view shown hosts, as this frame reads them and as the boxes were built
  // from, each view's box listing its top nodes.
  std::vector<const VP::View *> _hosted;
  std::vector<const VP::View *> _built_hosted;
  std::string _built_inside;
  std::string _built_folder;
  std::uint64_t _built_changes = 0;
  Shown _built_layers = every_layer;
  Layout _layout;
  glm::uvec2 _laid_cell{};
  bool _laid = false;
  // The nodes selected, by name, in the view named, which select may name before the
  // graph shows it, as a script's does before the first frame.
  std::vector<std::string> _selection;
  std::string _selected_in;
  // The boxes raised by a press or select, by name, the last over the others, the order
  // the boxes are drawn in, and for each what of the boxes drawn after it hides it, kept
  // so a frame allocates nothing for them.
  std::vector<std::string> _raised;
  std::vector<std::size_t> _order;
  std::vector<VP_VIEW::Rect> _covers;
  std::vector<VP_VIEW::Rect> _over;
  Arranged _arranged;
  std::string _ini;      // where the view shown keeps what is arranged; empty for none
  bool _refused = false; // its .ini holds a mistake, which no save writes over
  PanZoom _pan_zoom;
  float _home_zoom = 1.0f; // as near as home is, past half of which zooming out goes up
  // Until the frame home is worked out in, which needs the boxes laid out in the area.
  bool _homing = false;
  // The box a level up was left from, which the level shown next shows in focus: a node's
  // or a view's name, or a folder, as an absolute path; empty for none.
  std::string _left;
  std::optional<Grab> _grab;
  // Where a press on the ground with shift began the select box, until it is let go.
  std::optional<glm::vec2> _select_from;
  glm::vec2 _pointer{};   // as the events so far left it
  Under _hovered;         // what the pointer is over, once the frame laid the boxes out
  bool _dragging = false; // the ground, which the graph follows
  bool _shift = false;    // held, so a press adds a box to the selection or takes it out
};

} // namespace
