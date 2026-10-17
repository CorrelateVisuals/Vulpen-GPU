#pragma once

#include "contracts/Item.h"
#include "contracts/Rect.h"
#include "contracts/Relation.h"
#include "runtime/View.h"
#include "shared/PanZoom.h"
#include "Boxes.h"
#include "Layout.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: which curve the pointer is over,
// and the tip that tells a developer what that curve stands for.
namespace {

constexpr float least_reach = 32.0f;    // a bend's handles, at least, in pixels
constexpr float curve_reach = 4.0f;     // how near the pointer comes to be over a curve
constexpr std::size_t bend_pieces = 16; // a bend is measured in
// The rows a tip shows, at most, the last then saying how many more there are.
constexpr std::size_t tip_rows = 16;

// A bend's handles, as the curves it is drawn as give them: reaching half the way
// across, and at least least_reach, so one running back to the left still bends round.
glm::vec2 bend_reach(glm::vec2 from, glm::vec2 to) {
  return {std::max(std::abs(to.x - from.x) / 2.0f, least_reach), 0.0f};
}

// How far a point is from a straight piece of a curve.
float away(glm::vec2 point, glm::vec2 from, glm::vec2 to) {
  const glm::vec2 along = to - from;
  const float length = glm::dot(along, along);
  const float t =
      length > 0.0f ? std::clamp(glm::dot(point - from, along) / length, 0.0f, 1.0f) : 0.0f;
  return glm::length(point - (from + along * t));
}

// The end whose curve is nearest a point on screen, within curve_reach pixels, measured
// as drawn: along its lines, and along a bend in pieces; of two as near, the one drawn
// later, as it shows over the other; none for none. The graph shows at the area's
// offset, moved and zoomed.
std::size_t curve_near(std::span<const Route> routes,
                       std::span<const Waypoint> waypoints,
                       const PanZoom &pan_zoom,
                       glm::vec2 offset,
                       glm::vec2 point) {
  std::size_t nearest = no_box;
  float least = curve_reach;
  for (std::size_t end = 0; end < routes.size(); ++end) {
    const std::span<const Waypoint> way =
        waypoints.subspan(routes[end].first, routes[end].count);
    for (std::size_t at = 0; at + 1 < way.size(); ++at) {
      const glm::vec2 from = offset + pan_zoom.on_screen(way[at].at);
      const glm::vec2 to = offset + pan_zoom.on_screen(way[at + 1].at);
      const glm::vec2 reach = bend_reach(from, to);
      const std::size_t pieces = way[at].bends ? bend_pieces : 1;
      glm::vec2 before = from;
      for (std::size_t piece = 1; piece <= pieces; ++piece) {
        const float t = static_cast<float>(piece) / static_cast<float>(pieces);
        const float s = 1.0f - t;
        const glm::vec2 next = way[at].bends ? s * s * s * from +
                                                   3.0f * s * s * t * (from + reach) +
                                                   3.0f * s * t * t * (to - reach) +
                                                   t * t * t * to
                                             : to;
        if (const float distance = away(point, before, next); distance <= least) {
          least = distance;
          nearest = end;
        }
        before = next;
      }
    }
  }
  return nearest;
}

// The cells a tip leaves either side of its rows inside the area, two a side.
constexpr std::int32_t tip_margin = 4;

// A tip's rows past room go, the last row saying how many there were: tip_rows at most,
// or as many as the area holds with a cell above and below them, two at least.
void cut_tip(VP_VIEW::Items &tip, std::size_t room) {
  room = std::max<std::size_t>(std::min(room, tip_rows), 2);
  if (tip.size() <= room)
    return;
  const std::size_t more = tip.size() - room + 1;
  tip.resize(room - 1);
  tip.push_back({.label = std::format("and {} more", more)});
}

// What the curve of an end stands for, a row an Item: a connection's name, its writer
// and its readers, as the manifest names them; or a relation's layer and the boxes it
// joins, then each relation it draws, its file, named from base, the line that says it,
// and what there names the other.
void tell(VP_VIEW::Items &tip,
          const Boxes &boxes,
          std::size_t end,
          const VP::View &flat,
          std::span<const VP_VIEW::Relation> relations,
          const std::filesystem::path &base) {
  const End &told = boxes.ends[end];
  if (told.layer == VP_VIEW::Layer::flow) {
    if (told.wire >= flat.connections.size())
      return;
    const VP::Connection &connection = flat.connections[told.wire];
    tip.push_back({.label = std::format("connection {}", connection.name)});
    tip.push_back({.label = std::format("from {}", connection.from.text())});
    for (const VP::Endpoint &reader : connection.to)
      tip.push_back({.label = std::format("to {}", reader.text())});
    return;
  }
  const auto title = [&](std::size_t box, std::size_t row) {
    return row == title_row
               ? boxes.boxes[box].title
               : rows_in(boxes, box)[row].text + rows_in(boxes, box)[row].written;
  };
  tip.push_back(
      {.label = std::format("{}: {} to {}",
                            VP_VIEW::layer_names[static_cast<std::size_t>(told.layer)],
                            title(told.from, told.from_row),
                            title(told.to, told.to_row))});
  for (const std::size_t at : boxes.told[end])
    if (at < relations.size())
      tip.push_back(
          {.label = std::format(
               "{}:{} {}",
               std::filesystem::path(relations[at].from).lexically_proximate(base).generic_string(),
               relations[at].line,
               relations[at].said)});
}

// The names a box lists past listed_room, two blanks apart, as many a row as fit in width
// characters, a name too long for one on a row of its own.
void tell_more(VP_VIEW::Items &tip, std::span<const std::string> names, std::size_t width) {
  for (const std::string &name : names) {
    if (!tip.empty() && tip.back().label.size() + 2 + name.size() <= width)
      tip.back().label.append("  ").append(name);
    else
      tip.push_back({.label = name});
  }
}

// Where a tip shows, its rows in cells of the font's own size: beside the pointer, below
// it and to its right, or where that leaves the area, above it or to its left, kept a
// cell inside the area, and as wide as its widest row, or the area, which cuts the rows
// past it.
VP_VIEW::Rect tip_place(std::span<const VP_VIEW::Item> tip,
                        glm::ivec2 cell,
                        glm::vec2 pointer,
                        const VP_VIEW::Rect &area) {
  std::size_t widest = 0;
  for (const VP_VIEW::Item &row : tip)
    widest = std::max(widest, row.label.size());
  const glm::ivec2 at_pointer(glm::round(pointer));
  const glm::ivec2 low = area.offset + cell;
  const glm::ivec2 high = area.offset + glm::ivec2(area.extent) - cell;
  const glm::ivec2 size(
      std::min(static_cast<std::int32_t>(widest) * cell.x, std::max(high.x - low.x, 0)),
      static_cast<std::int32_t>(tip.size()) * cell.y);
  glm::ivec2 at = at_pointer + glm::ivec2(2 * cell.x, cell.y);
  if (at.x + size.x > high.x)
    at.x = at_pointer.x - 2 * cell.x - size.x;
  if (at.y + size.y > high.y)
    at.y = at_pointer.y - cell.y - size.y;
  return {.offset = glm::max(at, low), .extent = glm::uvec2(size)};
}

} // namespace
