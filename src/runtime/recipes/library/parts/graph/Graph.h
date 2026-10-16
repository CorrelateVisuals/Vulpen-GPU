#pragma once

#include "contracts/Curve.h"
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "contracts/Typed.h"
#include "runtime/Operator.h"
#include "shared/Labels.h"
#include "shared/PanZoom.h"
#include "Arranged.h"

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

// A node, or a view the view shown hosts, where the graph shows it: its box, from the
// graph's top left before it moves or zooms, a row for its name and one for each of its
// files, and its column, counted from the boxes that read nothing.
struct Placed {
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  std::size_t column = 0;
};

// A row a connection keeps in a column it passes, so it runs past that column's boxes,
// not behind them: as wide as the column, from the graph's top left before it moves.
struct Lane {
  std::size_t pair = 0; // of the connection's writer and reader
  std::size_t column = 0;
  glm::ivec2 offset{};
  std::int32_t width = 0;
};

// A box pressed: which, where the pointer was then, the boxes a drag moves, each with
// where it was then, and whether it has been dragged, or is a press let go where it was.
struct Grab {
  std::size_t box = 0;
  glm::vec2 from{};
  std::vector<std::pair<std::size_t, glm::ivec2>> moving;
  bool moved = false;
};

// The graph of the current view, as the terminals' lines go to it, which view switch
// moves and a second press on a view's box moves into it: a box a node, with its name and
// its files, a box each view it hosts, with its name and the file it is hosted from, and
// a curve a connection, from the writer's right side to the reader's left, a view's from
// where its host takes its window. The manifest holds no places (V04), so the graph alone
// lays it out: a node a column past every node it reads from, down each column in the
// manifest's order, and below its boxes a lane for each connection that passes the
// column. A box dragged stays where it is put, and the first drag places every box where
// it shows, so a change to the graph moves none; the boxes placed are a look at the
// graph, not its plumbing, so they go in .vulpen/graph.ini beside the view's manifest,
// and a restart shows them as they were left; so do the colors a person gives each kind
// of node there, which draw its boxes' borders and names. The graph starts at home, as a
// view is shown and as h brings it there: unmoved at zoom 1 while every box shows whole
// there, else moved and zoomed out until they all do. A drag on the ground moves the
// graph and the wheel zooms it about the pointer, as the Perform panel's image moves, and
// neither is kept. A press on a box selects it, shift adding it or taking it out, and
// opens the file of a file's row in the editor; a drag on a box selected moves every box
// selected. With shift, a drag on the ground draws a select box, and letting go adds
// every box it touches to the selection.
// It owns the selection, of nodes and of the views the view shown hosts, which select and
// selected reach as lines do (V06), and which delete or backspace removes as node remove
// and view close do, and
// takes keys from the keys part while it has the focus, which a press in its area gives
// it. Its own shaders draw the ground, lines that move and zoom with the graph over the
// whole area, as the POC's grid did; it writes Rects, Labels and Curves for the parts
// that draw over them, and only what lies in its area: a box cut at its edge, and a row
// of text with the area as its room, which glyphs cuts at the edge; a curve goes whole,
// and the curves part cuts it at the edge of the ground the graph hands it.
class Graph final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;
  void command(VP::Call &call) override;

private:
  void key(VP::Cook &frame, std::string_view key);
  void show(VP::Cook &frame, const VP::View *view, std::string_view name);
  void name_views(const VP::View &view);
  void lay_out(const VP::View &view, glm::uvec2 cell);
  void place_columns(glm::uvec2 cell);
  void home(glm::uvec2 cell);
  void
  follow(VP::Cook &frame, const VP::View *view, const VP::Event &event, glm::uvec2 cell);
  void press(VP::Cook &frame, const VP::View &view, std::uint32_t presses);
  void drag(const VP::View &view, glm::vec2 to);
  void release(VP::Cook &frame, const VP::View *view, glm::uvec2 cell);
  void open(VP::Cook &frame, const VP::View &view, std::size_t at, glm::uvec2 cell);
  void turn(glm::vec2 turn);
  bool selected(std::string_view node) const;
  void select(VP::CommandPort &commands, std::span<const std::string> nodes) const;
  void
  select_touched(VP::CommandPort &commands, const VP::View &view, glm::vec2 from) const;
  void erase(VP::Cook &frame, const VP::View &view);
  void save(VP::Cook &frame);
  void keep(VP::FilePort &files);
  void write(VP::Cook &frame, const VP::View &view, glm::uvec2 cell);
  void write_select_box(std::span<VP_VIEW::Rect> rects, std::uint32_t &written) const;
  void write_labels(VP::Cook &frame, const VP::View &view, glm::uvec2 cell);
  void write_row(glm::vec2 at,
                 std::string_view row,
                 std::uint32_t role,
                 glm::uvec2 cell,
                 Labels &written) const;
  void write_curves(VP::Cook &frame, glm::uvec2 cell);
  void write_curve(std::span<VP_VIEW::Curve> curves,
                   std::uint32_t &written,
                   glm::vec2 from,
                   glm::vec2 to) const;
  // Where a place of the graph shows this frame, in pixels from the window's top left.
  glm::vec2 on_screen(glm::vec2 at) const;
  // A box's name, the rows under it and its color: a node's, or past the nodes those of
  // a view the view hosts, in the order hosted.
  const std::string &name_of(const VP::View &view, std::size_t box) const;
  std::span<const std::string> rows_of(const VP::View &view, std::size_t box) const;
  std::optional<std::uint32_t> color_of(const VP::View &view, std::size_t box) const;
  VP_VIEW::Rect box(const Placed &placed) const;
  VP_VIEW::Rect select_box(glm::vec2 from) const;

  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
  const VP_VIEW::Typed *_typed = nullptr;
  std::string _name; // as the focus names it
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
  // By the view's nodes, its columns and its connections, kept between frames so a frame
  // allocates nothing once they hold the graph shown.
  std::vector<Placed> _placed;
  std::vector<std::int32_t> _lefts; // each column's left; its width while laid out
  std::vector<std::int32_t> _downs; // each column's next row
  std::vector<std::size_t> _ends;   // a connection's writer and reader, two a pair
  std::vector<Lane> _lanes;         // a pair's in the order of its columns
  std::string _shown; // the view the tab names; empty for the one vulpen started with
  // The file each view the view shown hosts is hosted from, and the row its box shows of
  // it, found again only when they change, so a frame allocates nothing for them.
  std::vector<std::filesystem::path> _view_files;
  std::vector<std::string> _view_rows;
  // The nodes selected, by name, in the view named, which select may name before the
  // graph shows it, as a script's does before the first frame.
  std::vector<std::string> _selection;
  std::string _selected_in;
  Arranged _arranged;
  std::string _ini;      // where the view shown keeps what is arranged; empty for none
  bool _refused = false; // its .ini holds a mistake, which no save writes over
  PanZoom _pan_zoom;
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
