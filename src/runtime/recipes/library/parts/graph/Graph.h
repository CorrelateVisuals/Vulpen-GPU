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
struct Grab {
  std::size_t box = 0;
  glm::vec2 from{};
  std::vector<std::pair<std::size_t, glm::ivec2>> moving;
  bool moved = false;
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
// stays where it is put, and the first drag places every box where it shows, so a change
// to the graph moves none; the boxes placed are a look at the graph, not its plumbing, so
// they go in .vulpen/graph.ini beside the view's manifest, and a restart shows them as
// they were left; so do the colors a person gives each kind of node there, which draw its
// boxes' borders and names. The graph starts at home, as a view or a level is shown and
// as h brings it there: unmoved at zoom 1 while every box shows whole there, else moved
// and zoomed out until they all do. A drag on the ground moves the graph and the wheel
// zooms it about the pointer, as the Perform panel's image moves, and neither is kept. A
// press on a box selects it, shift adding it or taking it out, and opens the file of a
// row that names one in the editor; a drag on a box selected moves every box selected.
// With shift, a drag on the ground draws a select box, and letting go adds every box it
// touches to the selection.
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
  void show(VP::Cook &frame, const VP::View *view, std::string_view name);
  void build(const VP::View *edited, const VP::View *flat);
  void name_tab();
  void show_view(VP::Cook &frame) const;
  void home(glm::uvec2 cell);
  void follow(VP::Cook &frame, bool room, const VP::Event &event, glm::uvec2 cell);
  void press(VP::Cook &frame, std::uint32_t presses);
  void drag(glm::vec2 to);
  void release(VP::Cook &frame, bool room, glm::uvec2 cell);
  void open(VP::Cook &frame, std::size_t at, glm::uvec2 cell);
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
  void write_select_box(std::span<VP_VIEW::Rect> rects, std::uint32_t &written) const;
  void write_labels(VP::Cook &frame, glm::uvec2 cell);
  void write_row(glm::vec2 at,
                 std::string_view row,
                 std::uint32_t role,
                 glm::uvec2 cell,
                 Labels &written) const;
  void write_curves(VP::Cook &frame);
  float corner(std::span<const Waypoint> way, std::size_t at) const;
  // Where a place of the graph shows this frame, in pixels from the window's top left.
  glm::vec2 on_screen(glm::vec2 at) const;
  // The middle of a box's port row, at its left edge or its right, on screen.
  glm::vec2 port_of(std::size_t box, std::size_t row, bool right) const;
  // The box a press on it selects: its own, or that of the node whose recipe holds it.
  std::size_t own_box(std::size_t box) const;
  std::optional<std::uint32_t> color_of(std::size_t box) const;
  VP_VIEW::Rect box(std::size_t at) const;
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
  Shown _layers = every_layer;        // as its relations param names them
  std::string _shown; // the view the tab names; empty for the one vulpen started with
  // The boxes of the view shown, built again only when the views they were built from
  // change, which each edit replaces, and laid out again only then, as the font's cell
  // changes, or as a box is dragged, so a frame allocates nothing for them.
  Boxes _boxes;
  const VP::View *_built_edited = nullptr;
  const VP::View *_built_flat = nullptr;
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
  Arranged _arranged;
  std::string _ini;      // where the view shown keeps what is arranged; empty for none
  bool _refused = false; // its .ini holds a mistake, which no save writes over
  PanZoom _pan_zoom;
  float _home_zoom = 1.0f; // as near as home is, past half of which zooming out goes up
  // Until the frame home is worked out in, which needs the boxes laid out in the area.
  bool _homing = false;
  std::optional<Grab> _grab;
  // Where a press on the ground with shift began the select box, until it is let go.
  std::optional<glm::vec2> _select_from;
  glm::vec2 _pointer{};   // as the events so far left it
  bool _dragging = false; // the ground, which the graph follows
  bool _shift = false;    // held, so a press adds a box to the selection or takes it out
};

} // namespace
