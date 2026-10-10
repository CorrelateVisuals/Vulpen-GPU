#pragma once

#include "contracts/Curve.h"
#include "contracts/Font.h"
#include "contracts/Item.h"
#include "contracts/Label.h"
#include "contracts/Palette.h"
#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Only Graph.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the part in one binary never clash.
namespace {

// A node where the graph shows it: its box, from the graph's top left before it moves,
// and its column, counted from the nodes that read nothing.
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

// The graph of the view its view hosted most recently, as the terminal's lines go to it:
// a box a node, with its name, and a curve a connection, from the writer's right side to
// the reader's left. The manifest holds no places (V04), so the graph alone lays it out:
// a node a column past every node it reads from, down each column in the manifest's
// order, and below its boxes a lane for each connection that passes the column. A drag
// on the ground or the wheel moves the graph, and how far stays with the part, as an
// editor's scroll does; a press on a box opens the node's first file in the editor. It
// writes Rects, Labels and Curves and draws nothing itself, and only what lies in its
// area: a box cut at its edge, a name cut to the characters inside, and a curve whose
// ends both lie inside.
class Graph final : public VP::Operator {
public:
  void bind(VP::Bind &node) override;
  void cook(VP::Cook &frame) override;

private:
  void lay_out(const VP::View &view, glm::uvec2 cell);
  void place_columns(glm::uvec2 cell);
  void follow(VP::Cook &frame, const VP::View *view, const VP::Event &event);
  void press(VP::Cook &frame, const VP::View &view);
  void write(VP::Cook &frame, const VP::View &view, glm::uvec2 cell);
  std::uint32_t write_dots(std::span<VP_VIEW::Rect> rects) const;
  void write_labels(VP::Cook &frame, const VP::View &view, glm::uvec2 cell);
  void write_curves(VP::Cook &frame);
  void write_curve(std::span<VP_VIEW::Curve> curves,
                   std::uint32_t &written,
                   glm::vec2 from,
                   glm::vec2 to) const;
  void name_tab(std::string_view shown);
  // A node's box where it shows this frame, in pixels from the window's top left.
  VP_VIEW::Rect box(const Placed &placed) const;

  const VP_VIEW::Rect *_area = nullptr;
  const VP_VIEW::Font *_font = nullptr;
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
  std::string _shown;               // the view the tab names; empty for none
  glm::vec2 _pan{};                 // how far the graph moved from where it starts
  glm::vec2 _pointer{};             // as the events so far left it
  bool _dragging = false;
};

} // namespace
