#pragma once

#include "Arranged.h"
#include "Boxes.h"

#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: where each box shows, which the
// graph alone works out, but for the boxes a person placed, and the way each curve runs.
// Both are worked out together, a level at a time, as a level's lanes widen its gaps and
// its tracks push its boxes down, so the file runs a little past 800 lines (CPP12).
namespace {

constexpr std::int32_t pad = 4;         // in pixels, above and below a box's rows
constexpr std::int32_t column_gap = 4;  // in cells, between columns besides their lanes
constexpr std::int32_t name_margin = 1; // in cells, either side of a row in its box
constexpr std::size_t no_gap = static_cast<std::size_t>(-1); // a track's, in a channel

// Where the graph alone puts its first box: half a column's gap in from the left and a
// row down, so a home that fits shows the boxes as far in.
glm::ivec2 margin_of(glm::uvec2 cell) {
  return {static_cast<std::int32_t>(column_gap * cell.x) / 2,
          static_cast<std::int32_t>(cell.y)};
}

// The middle of a box's row from its top, its name's row before the first, where a
// relation ends.
float row_y(std::size_t row, glm::uvec2 cell) {
  const float rows = row == title_row ? -1.0f : static_cast<float>(row);
  return static_cast<float>(pad) + (rows + 1.5f) * static_cast<float>(cell.y);
}

// Where a box shows at zoom 1, and how large it is with the boxes inside it.
struct Placed {
  // From the top left of the boxes inside the box it is in, below that box's rows, or
  // from the graph's top left for a box at the top.
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  glm::uvec2 least{};      // as large as its rows and the boxes inside it, at least
  glm::ivec2 at{};         // from the graph's top left
  std::int32_t inside = 0; // from its top, where the boxes inside it start
  bool frames = false;     // it holds boxes, which show through below its rows
  std::size_t column = 0;  // among the boxes inside the box it is in
};

// A piece of a curve at one level, the box holding both its ends there or the top: from
// the box inside the level holding its writer to the one holding its reader, or from the
// level's left edge, where it enters the level's box, or to its right edge, where it
// leaves it. Back when it runs against the columns, as one closing a cycle does.
struct Leg {
  std::size_t end = 0;
  std::size_t level = no_box;
  std::size_t writer = no_box; // no_box: the level's left edge
  std::size_t reader = no_box; // no_box: the level's right edge
  bool back = false;
  // The gap it turns in after its writer, a gap before each column and one after the
  // last, and the one before its reader, the same where it turns once.
  std::size_t out = 0;
  std::size_t in = 0;
};

// Where a connection crosses the edge of a box, from its top: into it at its left and
// out of it at its right, once each, so the curves to its readers inside share the way.
struct Crossing {
  std::size_t box = 0;
  std::size_t wire = 0;
  bool out = false;
  float y = 0.0f;
};

// A connection's lane in a gap of a level, how far across from where the boxes inside the
// level start; or its track in the channel above the level's columns or below them, how
// far down from there. Its curves share it.
struct Lane {
  std::size_t level = no_box;
  std::size_t wire = 0;
  std::size_t gap = no_gap;
  float at = 0.0f;
};

// A place a curve passes, from the graph's top left at zoom 1, and how it goes on to the
// next: straight, or, from a writer holding its reader or the other way round, bending
// as a wire does, leaving heading right and arriving from the left.
struct Waypoint {
  glm::vec2 at{};
  bool bends = false;
};

// An end's waypoints, among all of them.
struct Route {
  std::size_t first = 0;
  std::size_t count = 0;
};

// A connection at the level being laid out: the height it leaves at, or first reaches
// where it enters the level's box, which picks the channel nearer it; and, if a leg of it
// turns in two gaps, its track, the gaps the track spans, its channel and its number.
struct Wire {
  std::size_t wire = 0;
  float from = 0.0f;
  bool tracked = false;
  std::size_t first = 0;
  std::size_t last = 0;
  bool above = false;
  std::size_t track = 0;
};

// A connection's turn in a gap, as the lanes there are ordered: the heights it comes to
// the lane from at the lane's left and goes on to at its right, summed, then their
// means, and how far it spans.
struct Turn {
  std::size_t wire = 0;
  std::size_t gap = 0;
  float left = 0.0f;
  std::uint32_t lefts = 0;
  float right = 0.0f;
  std::uint32_t rights = 0;
  float low = 0.0f;
  float high = 0.0f;
};

// The graph's layout: each box as large as its rows and the boxes inside it, and those
// in columns, a box a column past every box beside it that it reads from, down each
// column in the manifest's order; a box a person placed where they put it. And each
// curve's way: at each level it passes, it turns only in the lanes of the gaps between
// columns and runs past a column only along a track of the channel above the columns or
// below them, the gaps and the channels as wide as what they hold, and it crosses into
// a box and out of it at the box's edges; so it runs over no box and no other's lane.
class Layout {
public:
  void lay_out(const Boxes &boxes, const Arranged &arranged, glm::uvec2 cell) {
    const std::size_t count = boxes.boxes.size();
    _placed.assign(count, {});
    // Cleared, not made anew, so a layout allocates nothing once they grew (CPP10).
    _children.resize(count + 1); // the top's last
    for (std::vector<std::size_t> &inner : _children)
      inner.clear();
    for (std::size_t box = 0; box < count; ++box)
      children(boxes.boxes[box].parent).push_back(box);
    legs(boxes);
    _crossings.clear();
    _lanes.clear();
    // Each box after the box it is inside, so going back, the boxes inside one come
    // before it, sized before it is, and where a curve crosses its edge is known.
    for (std::size_t box = count; box-- > 0;)
      arrange(boxes, box, arranged, cell);
    arrange(boxes, no_box, arranged, cell);
    for (std::size_t box = 0; box < count; ++box) {
      const std::size_t parent = boxes.boxes[box].parent;
      _placed[box].at =
          parent == no_box ? _placed[box].offset
                           : _placed[parent].at + content_of(parent, cell) +
                                 _placed[box].offset;
    }
    route(boxes, cell);
  }

  std::span<const Placed> placed() const {
    return _placed;
  }
  // A curve's way, by its end, through the waypoints.
  std::span<const Route> routes() const {
    return _routes;
  }
  std::span<const Waypoint> waypoints() const {
    return _waypoints;
  }
  // Where the boxes inside a box start, from its top left.
  glm::ivec2 content_of(std::size_t box, glm::uvec2 cell) const {
    return {name_margin * static_cast<std::int32_t>(cell.x), _placed[box].inside};
  }

private:
  std::vector<std::size_t> &children(std::size_t box) {
    return _children[box == no_box ? _children.size() - 1 : box];
  }

  // Each end's legs in the order it runs: out of each box holding its writer, up to the
  // box holding both ends, across it, and into each box holding its reader. An end whose
  // writer holds its reader, or the other way round, has none, and bends from port to
  // port. Then which legs between two boxes run against the columns: a depth-first walk
  // from each box in turn finds those leading back to a box it is still walking from.
  void legs(const Boxes &boxes) {
    _legs.clear();
    for (std::size_t end = 0; end < boxes.ends.size(); ++end) {
      const End &ends = boxes.ends[end];
      const auto [writer, reader] = siblings(boxes, ends.from, ends.to);
      if (writer == no_box)
        continue;
      for (std::size_t box = ends.from; box != writer; box = boxes.boxes[box].parent)
        _legs.push_back({.end = end, .level = boxes.boxes[box].parent, .writer = box});
      _legs.push_back({.end = end,
                       .level = boxes.boxes[writer].parent,
                       .writer = writer,
                       .reader = reader});
      const auto into = static_cast<std::ptrdiff_t>(_legs.size());
      for (std::size_t box = ends.to; box != reader; box = boxes.boxes[box].parent)
        _legs.push_back({.end = end, .level = boxes.boxes[box].parent, .reader = box});
      std::reverse(_legs.begin() + into, _legs.end());
    }
    _walked.assign(boxes.boxes.size(), Walk::not_yet);
    for (std::size_t box = 0; box < boxes.boxes.size(); ++box)
      if (_walked[box] == Walk::not_yet)
        walk(box);
  }

  // The boxes holding a writer and a reader inside the box holding both, or the top; none
  // where one holds the other.
  static std::pair<std::size_t, std::size_t>
  siblings(const Boxes &boxes, std::size_t writer, std::size_t reader) {
    for (std::size_t from = writer; from != no_box; from = boxes.boxes[from].parent)
      for (std::size_t to = reader; to != no_box; to = boxes.boxes[to].parent)
        if (from != to && boxes.boxes[from].parent == boxes.boxes[to].parent)
          return {from, to};
    return {no_box, no_box};
  }

  enum class Walk : std::uint8_t { not_yet, walking, walked };

  void walk(std::size_t box) {
    _walked[box] = Walk::walking;
    for (Leg &leg : _legs)
      if (leg.writer == box && leg.reader != no_box) {
        if (_walked[leg.reader] == Walk::walking)
          leg.back = true;
        else if (_walked[leg.reader] == Walk::not_yet)
          walk(leg.reader);
      }
    _walked[box] = Walk::walked;
  }

  // The boxes inside one, in columns or where a person put them, and its own size: as
  // wide as its widest row with a cell either side, and as tall as its rows with pad
  // above and below them, then the boxes inside it, with half a row around them; or
  // larger, as a person made it.
  void arrange(const Boxes &boxes,
               std::size_t box,
               const Arranged &arranged,
               glm::uvec2 cell) {
    const std::vector<std::size_t> &inner = children(box);
    if (box == no_box) {
      if (!inner.empty())
        columns(boxes, box, inner, arranged, cell);
      return;
    }
    const auto margin = static_cast<std::int32_t>(name_margin * cell.x);
    std::size_t widest = boxes.boxes[box].title.size();
    for (const BoxRow &row : rows_in(boxes, box))
      widest = std::max(widest,
                        row.text.size() + row.written.size() +
                            (row.text.empty() || row.written.empty() ? 0 : 1));
    const auto rows = static_cast<std::int32_t>(1 + boxes.boxes[box].rows);
    const std::int32_t apart = inner.empty() ? 0 : static_cast<std::int32_t>(cell.y) / 2;
    // Known before the boxes inside it are placed, as where a curve crosses its edge is
    // from its top.
    _placed[box].inside = rows * static_cast<std::int32_t>(cell.y) + 2 * pad + apart;
    const glm::ivec2 room =
        inner.empty() ? glm::ivec2(0) : columns(boxes, box, inner, arranged, cell);
    Placed &placed = _placed[box];
    placed.frames = !inner.empty();
    placed.least = glm::uvec2(
        std::max(static_cast<std::int32_t>(widest) * static_cast<std::int32_t>(cell.x),
                 room.x) +
            2 * margin,
        placed.inside + (inner.empty() ? 0 : room.y + apart));
    placed.extent = placed.least;
    if (const auto sized = arranged.sizes.find(boxes.boxes[box].name);
        sized != arranged.sizes.end())
      placed.extent = glm::max(placed.extent, glm::uvec2(glm::max(sized->second, 0)));
  }

  // A box a column past every box beside it that it reads from, each column as wide as
  // its widest box, its boxes one under the other, a row of text apart, below the
  // channel above them, and each gap as wide as its lanes and a column's gap. Where a
  // person placed a box, each shows where it was placed or the graph alone put it, and
  // the columns the curves keep to are the runs of boxes whose widths meet, left to
  // right. Returns how far they reach.
  glm::ivec2 columns(const Boxes &boxes,
                     std::size_t level,
                     const std::vector<std::size_t> &inner,
                     const Arranged &arranged,
                     glm::uvec2 cell) {
    std::size_t count = order(level, inner);
    const glm::ivec2 start = level == no_box ? margin_of(cell) : glm::ivec2(0);
    stack(inner, count, cell, start);
    const bool moved = std::ranges::any_of(inner, [&](std::size_t box) {
      return arranged.places.contains(boxes.boxes[box].name);
    });
    std::int32_t below = moved ? 0 : channels(boxes, level, count, cell, start, false);
    widen(level, count, cell, start);
    place(boxes, level, inner, arranged);
    if (moved) {
      count = runs(inner);
      below = channels(boxes, level, count, cell, start, true);
      spread(level, count, cell);
    }
    glm::ivec2 reach{_lefts.back() + _widths.back() + _gaps.back(), below};
    for (const std::size_t box : inner)
      reach = glm::max(reach, _placed[box].offset + glm::ivec2(_placed[box].extent));
    return reach;
  }

  // Each box at its column's left, or where a person placed it. Inside a box, a place
  // stays inside it, as a drag keeps it, though an .ini written before the box held it
  // may say otherwise.
  void place(const Boxes &boxes,
             std::size_t level,
             const std::vector<std::size_t> &inner,
             const Arranged &arranged) {
    for (const std::size_t box : inner) {
      Placed &placed = _placed[box];
      placed.offset.x = _lefts[placed.column];
      if (const auto found = arranged.places.find(boxes.boxes[box].name);
          found != arranged.places.end())
        placed.offset =
            level == no_box ? found->second : glm::max(found->second, glm::ivec2(0));
    }
  }

  // Each column as wide as its widest box, and its boxes one under the other, a row of
  // text apart.
  void stack(const std::vector<std::size_t> &inner,
             std::size_t count,
             glm::uvec2 cell,
             glm::ivec2 start) {
    _widths.assign(count, 0);
    _downs.assign(count, start.y);
    for (const std::size_t box : inner) {
      Placed &placed = _placed[box];
      _widths[placed.column] =
          std::max(_widths[placed.column], static_cast<std::int32_t>(placed.extent.x));
      placed.offset.y = _downs[placed.column];
      _downs[placed.column] += static_cast<std::int32_t>(placed.extent.y + cell.y);
    }
  }

  // Where a person placed boxes, each box's column the run of boxes, left to right, whose
  // widths meet, so the gaps between the runs are free; each run's left and width.
  // Returns how many runs there are.
  std::size_t runs(const std::vector<std::size_t> &inner) {
    _sorted.assign(inner.begin(), inner.end());
    std::ranges::sort(_sorted, {}, [&](std::size_t box) {
      return _placed[box].offset.x;
    });
    _lefts.clear();
    _widths.clear();
    std::int32_t right = 0;
    for (const std::size_t box : _sorted) {
      Placed &placed = _placed[box];
      if (_lefts.empty() || placed.offset.x >= right) {
        _lefts.push_back(placed.offset.x);
        _widths.push_back(0);
      }
      right = std::max(_lefts.back() + _widths.back(),
                       placed.offset.x + static_cast<std::int32_t>(placed.extent.x));
      _widths.back() = right - _lefts.back();
      placed.column = _lefts.size() - 1;
    }
    return _lefts.size();
  }

  // Where a person placed boxes, each gap's lanes a lane apart in its middle, or spread
  // across it where it is narrower; before the first run and after the last, a lane
  // apart from it, the level reaching past those after.
  void spread(std::size_t level, std::size_t count, glm::uvec2 cell) {
    const auto lane = static_cast<float>(cell.x);
    _gaps.assign(count + 1, 0); // its lanes, for now
    for (const Lane &placed : _lanes)
      if (placed.level == level && placed.gap != no_gap)
        ++_gaps[placed.gap];
    _ins.assign(count + 1, 0);
    for (Lane &placed : _lanes) {
      if (placed.level != level || placed.gap == no_gap)
        continue;
      const std::size_t gap = placed.gap;
      const auto lanes = static_cast<float>(_gaps[gap]);
      const auto nth = static_cast<float>(_ins[gap]++);
      if (gap == 0) {
        placed.at = static_cast<float>(_lefts.front()) - (lanes - nth) * lane;
        continue;
      }
      const auto left = static_cast<float>(_lefts[gap - 1] + _widths[gap - 1]);
      const float room =
          gap == count ? (lanes + 1.0f) * lane : static_cast<float>(_lefts[gap]) - left;
      const float step = std::min(lane, room / (lanes + 1.0f));
      placed.at = left + (room - (lanes - 1.0f) * step) / 2.0f + nth * step;
    }
    if (_gaps.back() != 0)
      _gaps.back() = (_gaps.back() + 1) * static_cast<std::int32_t>(cell.x);
  }

  // Each box a column past every box beside it that it reads from, a leg closing a cycle
  // ordering none. Returns how many columns there are.
  std::size_t order(std::size_t level, const std::vector<std::size_t> &inner) {
    for (std::size_t round = 0; round < inner.size(); ++round) {
      bool moved = false;
      for (const Leg &leg : _legs)
        if (leg.level == level && leg.writer != no_box && leg.reader != no_box &&
            !leg.back && _placed[leg.reader].column < _placed[leg.writer].column + 1) {
          _placed[leg.reader].column = _placed[leg.writer].column + 1;
          moved = true;
        }
      if (!moved)
        break;
    }
    std::size_t count = 0;
    for (const std::size_t box : inner)
      count = std::max(count, _placed[box].column + 1);
    return count;
  }

  // The level's connections, their tracks, the channel above the columns pushing the
  // boxes down, where they cross the level's edge, and their turns. Where a person placed
  // the boxes, they stay, and the tracks above them keep to the room there is, closer
  // where it is less, inside a box; the top has room above. Returns how far down the
  // channel below the boxes reaches, or nothing without one.
  std::int32_t channels(const Boxes &boxes,
                        std::size_t level,
                        std::size_t count,
                        glm::uvec2 cell,
                        glm::ivec2 start,
                        bool placed) {
    gather(boxes, level, count, cell);
    std::int32_t top = std::numeric_limits<std::int32_t>::max();
    std::int32_t bottom = std::numeric_limits<std::int32_t>::lowest();
    for (const std::size_t box : children(level)) {
      const Placed &placed = _placed[box];
      top = std::min(top, placed.offset.y);
      bottom = std::max(bottom,
                        placed.offset.y + static_cast<std::int32_t>(placed.extent.y));
    }
    const std::array<std::size_t, 2> tracks =
        assign(static_cast<float>(top + bottom) / 2.0f);
    const auto lane = static_cast<std::int32_t>(cell.x);
    const auto above = static_cast<std::int32_t>(tracks[1]);
    const std::int32_t over = placed || above == 0 ? 0 : (above + 1) * lane;
    for (const std::size_t box : children(level))
      _placed[box].offset.y += over;
    const float room = placed && level == no_box
                           ? static_cast<float>((above + 1) * lane)
                           : static_cast<float>(top + over - start.y);
    lay_tracks(level, top + over, bottom + over, cell, room);
    cross(boxes, level, cell);
    turns(boxes, level, cell);
    return tracks[0] == 0
               ? 0
               : bottom + over + static_cast<std::int32_t>(tracks[0] + 1) * lane;
  }

  // Each track's height: those above the boxes a lane apart, or closer where the room
  // above them is less, the first the farthest from them; those below a lane apart, the
  // first the nearest.
  void lay_tracks(std::size_t level,
                  std::int32_t top,
                  std::int32_t bottom,
                  glm::uvec2 cell,
                  float room) {
    const auto lane = static_cast<float>(cell.x);
    float above = 0.0f;
    for (const Wire &wire : _wires)
      if (wire.tracked && wire.above)
        above = std::max(above, static_cast<float>(wire.track + 1));
    const float step = std::min(lane, room / (above + 1.0f));
    for (const Wire &wire : _wires) {
      if (!wire.tracked)
        continue;
      const auto track = static_cast<float>(wire.track);
      const float at = wire.above
                           ? static_cast<float>(top) - (above - track + 0.5f) * step
                           : static_cast<float>(bottom) + (track + 1.0f) * lane;
      _lanes.push_back({.level = level, .wire = wire.wire, .at = at});
    }
  }

  // The gaps each leg of the level turns in, and a Wire a connection, tracked where a leg
  // of it turns in two gaps: to pass a column, to run back, or to cross the level's edge
  // past a column.
  void gather(const Boxes &boxes, std::size_t level, std::size_t count, glm::uvec2 cell) {
    _wires.clear();
    for (Leg &leg : _legs) {
      if (leg.level != level)
        continue;
      leg.out = leg.writer == no_box ? 0 : _placed[leg.writer].column + 1;
      leg.in = leg.reader == no_box ? count : _placed[leg.reader].column;
      const std::size_t wire = boxes.ends[leg.end].wire;
      auto found = std::ranges::find(_wires, wire, &Wire::wire);
      if (found == _wires.end())
        found = _wires.insert(found,
                              {.wire = wire,
                               .from = y_on(boxes, leg, leg.writer != no_box, cell),
                               .first = leg.out,
                               .last = leg.out});
      if (leg.out != leg.in) {
        found->tracked = true;
        found->first = std::min({found->first, leg.out, leg.in});
        found->last = std::max({found->last, leg.out, leg.in});
      }
    }
  }

  // Each tracked connection in the channel nearer the height it leaves at, above the
  // columns or below them, the farther from the boxes the farther it leaves from them,
  // sharing a track with another whose span it does not meet. Returns the tracks below
  // and above.
  std::array<std::size_t, 2> assign(float middle) {
    std::array<std::size_t, 2> tracks{};
    for (const bool above : {true, false}) {
      std::ranges::sort(_wires, [&](const Wire &a, const Wire &b) {
        return above ? a.from < b.from : a.from > b.from;
      });
      for (std::size_t at = 0; at < _wires.size(); ++at) {
        Wire &wire = _wires[at];
        if (!wire.tracked || (wire.from < middle) != above)
          continue;
        wire.above = above;
        const auto before = _wires.begin() + static_cast<std::ptrdiff_t>(at);
        while (std::any_of(_wires.begin(), before, [&](const Wire &other) {
          return other.tracked && other.above == above && other.track == wire.track &&
                 other.first <= wire.last && wire.first <= other.last;
        }))
          ++wire.track;
        tracks[above] = std::max(tracks[above], wire.track + 1);
      }
    }
    return tracks;
  }

  // Where each connection crossing the level's edge does, from the level's top: on its
  // track, or with none, at the height of the first box it reaches inside, or of the one
  // it leaves.
  void cross(const Boxes &boxes, std::size_t level, glm::uvec2 cell) {
    for (const Leg &leg : _legs) {
      if (leg.level != level || (leg.writer != no_box && leg.reader != no_box))
        continue;
      const std::size_t wire = boxes.ends[leg.end].wire;
      const bool out = leg.reader == no_box;
      if (std::ranges::any_of(_crossings, [&](const Crossing &crossing) {
            return crossing.box == level && crossing.wire == wire && crossing.out == out;
          }))
        continue;
      const auto found = std::ranges::find(_wires, wire, &Wire::wire);
      const float y =
          found->tracked ? lane_at(level, wire, no_gap) : y_on(boxes, leg, out, cell);
      _crossings.push_back({.box = level,
                            .wire = wire,
                            .out = out,
                            .y = static_cast<float>(_placed[level].inside) + y});
    }
  }

  // Each leg's turns: where it turns once, in its one gap from the height it leaves at
  // to its reader's; else in the gap after its writer to its track, and in the gap
  // before its reader from its track. A connection turns in a gap in one lane, which its
  // legs share, the lanes of each gap in the order before gives.
  void turns(const Boxes &boxes, std::size_t level, glm::uvec2 cell) {
    _turning.clear();
    for (const Leg &leg : _legs) {
      if (leg.level != level)
        continue;
      const std::size_t wire = boxes.ends[leg.end].wire;
      const float from = y_on(boxes, leg, true, cell);
      const float to = y_on(boxes, leg, false, cell);
      if (leg.out == leg.in) {
        if (from != to)
          turn(wire, leg.out, {from, false}, {to, true});
        continue;
      }
      const float track = lane_at(level, wire, no_gap);
      const bool ahead = leg.in > leg.out;
      if (leg.writer != no_box)
        turn(wire, leg.out, {from, false}, {track, ahead});
      if (leg.reader != no_box)
        turn(wire, leg.in, {track, !ahead}, {to, true});
    }
    for (Turn &turned : _turning) {
      turned.left /= static_cast<float>(std::max(turned.lefts, 1u));
      turned.right /= static_cast<float>(std::max(turned.rights, 1u));
    }
    std::ranges::sort(_turning, before);
    for (const Turn &turned : _turning)
      _lanes.push_back({.level = level, .wire = turned.wire, .gap = turned.gap});
  }

  // A gap's lanes in the order that crosses them the fewest: first those whose turns all
  // lie at its left, as where a curve running back turns, the narrower nearer the column
  // before; then those reached from the left and left for the right, one turning up the
  // further left the higher it comes from, one turning down the further right; then those
  // whose turns all lie at its right, the wider first.
  static bool before(const Turn &a, const Turn &b) {
    if (a.gap != b.gap)
      return a.gap < b.gap;
    if (side(a) != side(b))
      return side(a) < side(b);
    const float a_span = a.high - a.low;
    const float b_span = b.high - b.low;
    if (side(a) != 1)
      return side(a) == 0 ? a_span < b_span : a_span > b_span;
    const bool a_up = a.right < a.left;
    if (a_up != (b.right < b.left))
      return a_up;
    return a_up ? a.left < b.left : a.left > b.left;
  }

  // A height a lane is reached at, and whether at its right.
  struct Side {
    float y = 0.0f;
    bool right = false;
  };

  // A connection's turn in a gap, from one height to another, joined to its lane there.
  void turn(std::size_t wire, std::size_t gap, Side from, Side to) {
    auto found = std::ranges::find_if(_turning, [&](const Turn &turned) {
      return turned.wire == wire && turned.gap == gap;
    });
    if (found == _turning.end())
      found = _turning.insert(
          found, {.wire = wire, .gap = gap, .low = from.y, .high = from.y});
    for (const Side &at : {from, to}) {
      (at.right ? found->right : found->left) += at.y;
      ++(at.right ? found->rights : found->lefts);
      found->low = std::min(found->low, at.y);
      found->high = std::max(found->high, at.y);
    }
  }

  // Its turns all at its left, reached from the left and left for the right, or all at
  // its right.
  static int side(const Turn &turn) {
    return turn.rights == 0 ? 0 : turn.lefts == 0 ? 2 : 1;
  }

  // Each gap as wide as a column's gap and its lanes, or before the first column and
  // after the last, its lanes with a lane either side; each column's left; and each lane
  // in its gap, a lane apart, in the order its turns came.
  void widen(std::size_t level, std::size_t count, glm::uvec2 cell, glm::ivec2 start) {
    const auto lane = static_cast<std::int32_t>(cell.x);
    const auto base = [&](std::size_t gap, bool laned) {
      return gap == 0 || gap == count ? (laned ? lane : 0)
                                      : static_cast<std::int32_t>(column_gap * cell.x);
    };
    _ins.assign(count + 1, 0);
    for (const Lane &placed : _lanes)
      if (placed.level == level && placed.gap != no_gap)
        ++_ins[placed.gap];
    _gaps.assign(count + 1, 0);
    for (std::size_t gap = 0; gap <= count; ++gap)
      _gaps[gap] = base(gap, _ins[gap] != 0) + _ins[gap] * lane;
    _lefts.assign(count, 0);
    for (std::size_t column = 0; column < count; ++column) {
      const std::int32_t left =
          column == 0 ? start.x : _lefts[column - 1] + _widths[column - 1];
      _lefts[column] = left + _gaps[column];
    }
    _ins.assign(count + 1, 0);
    for (Lane &placed : _lanes)
      if (placed.level == level && placed.gap != no_gap) {
        const std::size_t gap = placed.gap;
        const std::int32_t left = gap == 0 ? start.x : _lefts[gap - 1] + _widths[gap - 1];
        placed.at = static_cast<float>(left + base(gap, true) / 2 + lane * _ins[gap]++) +
                    static_cast<float>(lane) / 2.0f;
      }
  }

  // Where a leg leaves its writer or reaches its reader, from the top of the boxes inside
  // its level: the end's port, or where the connection crosses the edge of the box
  // holding it, or of the level's own.
  float y_on(const Boxes &boxes, const Leg &leg, bool writes, glm::uvec2 cell) const {
    const End &end = boxes.ends[leg.end];
    const std::size_t box = writes ? leg.writer : leg.reader;
    if (box == no_box)
      return crossing_at(leg.level, end.wire, !writes) -
             static_cast<float>(_placed[leg.level].inside);
    const auto top = static_cast<float>(_placed[box].offset.y);
    if (box == (writes ? end.from : end.to))
      return top + row_y(writes ? end.from_row : end.to_row, cell);
    return top + crossing_at(box, end.wire, writes);
  }

  float crossing_at(std::size_t box, std::size_t wire, bool out) const {
    const auto found = std::ranges::find_if(_crossings, [&](const Crossing &crossing) {
      return crossing.box == box && crossing.wire == wire && crossing.out == out;
    });
    return found == _crossings.end() ? 0.0f : found->y;
  }

  float lane_at(std::size_t level, std::size_t wire, std::size_t gap) const {
    const auto found = std::ranges::find_if(_lanes, [&](const Lane &lane) {
      return lane.level == level && lane.wire == wire && lane.gap == gap;
    });
    return found == _lanes.end() ? 0.0f : found->at;
  }

  // Each end's waypoints, a leg after another, or straight from port to port, bending,
  // for one whose writer holds its reader, or the other way round.
  void route(const Boxes &boxes, glm::uvec2 cell) {
    _routes.assign(boxes.ends.size(), {});
    _waypoints.clear();
    std::size_t leg = 0;
    for (std::size_t end = 0; end < boxes.ends.size(); ++end) {
      const std::size_t first = _waypoints.size();
      if (leg == _legs.size() || _legs[leg].end != end) {
        const End &ends = boxes.ends[end];
        add({.at = port(ends.from, ends.from_row, true, cell), .bends = true}, first);
        add({.at = port(ends.to, ends.to_row, false, cell)}, first);
      }
      for (; leg < _legs.size() && _legs[leg].end == end; ++leg)
        way(boxes, _legs[leg], cell, first);
      _routes[end] = {.first = first, .count = _waypoints.size() - first};
    }
  }

  // A leg's waypoints, through its level's lanes and along its track.
  void way(const Boxes &boxes, const Leg &leg, glm::uvec2 cell, std::size_t first) {
    const glm::vec2 from = point_of(boxes, leg, true, cell);
    const glm::vec2 to = point_of(boxes, leg, false, cell);
    const std::size_t wire = boxes.ends[leg.end].wire;
    const glm::vec2 origin =
        leg.level == no_box
            ? glm::vec2(0.0f)
            : glm::vec2(_placed[leg.level].at + content_of(leg.level, cell));
    const auto across = [&](std::size_t gap) {
      return origin.x + lane_at(leg.level, wire, gap);
    };
    add({.at = from}, first);
    if (leg.out == leg.in && from.y != to.y) {
      add({.at = {across(leg.out), from.y}}, first);
      add({.at = {across(leg.out), to.y}}, first);
    } else if (leg.out != leg.in) {
      const float track = origin.y + lane_at(leg.level, wire, no_gap);
      if (leg.writer != no_box) {
        add({.at = {across(leg.out), from.y}}, first);
        add({.at = {across(leg.out), track}}, first);
      }
      if (leg.reader != no_box) {
        add({.at = {across(leg.in), track}}, first);
        add({.at = {across(leg.in), to.y}}, first);
      }
    }
    add({.at = to}, first);
  }

  // Where a leg leaves or arrives, from the graph's top left: on the right of the box it
  // leaves and the left of the one it reaches, at the end's port or where the connection
  // crosses the box's edge; or on the level's own edge, its left where it enters the
  // level's box and its right where it leaves it.
  glm::vec2 point_of(const Boxes &boxes,
                     const Leg &leg,
                     bool writes,
                     glm::uvec2 cell) const {
    const End &end = boxes.ends[leg.end];
    const std::size_t box = writes ? leg.writer : leg.reader;
    if (box == no_box) {
      const Placed &level = _placed[leg.level];
      return glm::vec2(level.at) +
             glm::vec2(writes ? 0.0f : static_cast<float>(level.extent.x),
                       crossing_at(leg.level, end.wire, !writes));
    }
    if (box == (writes ? end.from : end.to))
      return port(box, writes ? end.from_row : end.to_row, writes, cell);
    const Placed &placed = _placed[box];
    return glm::vec2(placed.at) +
           glm::vec2(writes ? static_cast<float>(placed.extent.x) : 0.0f,
                     crossing_at(box, end.wire, writes));
  }

  // The middle of a box's port row, at its left edge or its right.
  glm::vec2 port(std::size_t box, std::size_t row, bool right, glm::uvec2 cell) const {
    const Placed &placed = _placed[box];
    const float across = right ? static_cast<float>(placed.extent.x) : 0.0f;
    return glm::vec2(placed.at) + glm::vec2(across, row_y(row, cell));
  }

  // A waypoint after the route's last, merged with it where it adds no turn: the same
  // place, or on along the same straight line.
  void add(const Waypoint &point, std::size_t first) {
    const std::size_t count = _waypoints.size() - first;
    if (count > 0 && _waypoints.back().at == point.at) {
      _waypoints.back().bends = point.bends;
      return;
    }
    if (count > 1) {
      const Waypoint &before = _waypoints[_waypoints.size() - 2];
      const Waypoint &last = _waypoints.back();
      const glm::vec2 one = last.at - before.at;
      const glm::vec2 two = point.at - last.at;
      if (!before.bends && !last.bends &&
          ((one.y == 0.0f && two.y == 0.0f && (one.x > 0.0f) == (two.x > 0.0f)) ||
           (one.x == 0.0f && two.x == 0.0f && (one.y > 0.0f) == (two.y > 0.0f)))) {
        _waypoints.back() = point;
        return;
      }
    }
    _waypoints.push_back(point);
  }

  std::vector<Placed> _placed;
  std::vector<Leg> _legs; // each end's in the order it runs, end after end
  std::vector<std::vector<std::size_t>> _children; // by box, the top's last
  std::vector<Walk> _walked;
  std::vector<Crossing> _crossings;
  std::vector<Lane> _lanes;
  std::vector<Route> _routes;
  std::vector<Waypoint> _waypoints;
  // For the level being laid out: each column's width, left and next row, each gap's
  // width and the lanes placed in it so far, and the connections and turns there.
  std::vector<std::int32_t> _widths;
  std::vector<std::int32_t> _lefts;
  std::vector<std::int32_t> _downs;
  std::vector<std::int32_t> _gaps;
  std::vector<std::int32_t> _ins;
  std::vector<std::size_t> _sorted; // its boxes, left to right, where a person placed any
  std::vector<Wire> _wires;
  std::vector<Turn> _turning;
};

} // namespace
