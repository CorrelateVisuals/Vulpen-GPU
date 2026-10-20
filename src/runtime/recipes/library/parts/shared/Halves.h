#pragma once

#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// How a dock lays out, so the split and the bar parts lay it out, and the docking part
// finds where each panel and its tab strip show, alike (RV09), a seam dragged too: a
// split divides a Rect in two along an axis, the first taking its ratio of the room the
// seam between them leaves, a ratio of 0 or 1 giving one half the whole and the other
// none, with no seam, as a panel hidden; and a bar cuts rows of text off a Rect's edge, a
// pad either side of them.
constexpr std::int32_t seam_width = 4; // in pixels: the gap a press on a split grabs
constexpr std::uint32_t bar_pad = 4;   // in pixels, either side of a bar's text cells
// A ratio keeps the three decimals param set writes, while dragged too, so letting go
// moves nothing.
constexpr float ratio_steps = 1000.0f;

struct Halves {
  VP_VIEW::Rect first;
  VP_VIEW::Rect seam;
  VP_VIEW::Rect second;
};

// The area from start along the axis, length long, and whole across it.
inline VP_VIEW::Rect part_of(const VP_VIEW::Rect &area,
                             glm::length_t along,
                             std::int32_t start,
                             std::int32_t length) {
  VP_VIEW::Rect rect = area;
  rect.offset[along] += start;
  rect.extent[along] = static_cast<std::uint32_t>(length);
  return rect;
}

// Along the axis, 0 for x and 1 for y; the seam as wide as the area allows.
inline Halves halves(const VP_VIEW::Rect &area, glm::length_t along, float ratio) {
  const auto whole = static_cast<std::int32_t>(area.extent[along]);
  const std::int32_t seam =
      ratio > 0.0f && ratio < 1.0f ? std::min(seam_width, whole) : 0;
  const std::int32_t room = whole - seam;
  const auto first =
      static_cast<std::int32_t>(std::lround(static_cast<float>(room) * ratio));
  return {.first = part_of(area, along, 0, first),
          .seam = part_of(area, along, first, seam),
          .second = part_of(area, along, first + seam, room - first)};
}

// The ratio that puts the seam's middle at a point along the axis, as a drag moves it
// from the ratio it has; that ratio while the halves have no room.
inline float
ratio_at(const VP_VIEW::Rect &area, glm::length_t along, float ratio, float at) {
  const Halves split = halves(area, along, ratio);
  const auto room =
      static_cast<std::int32_t>(split.first.extent[along] + split.second.extent[along]);
  if (room == 0)
    return ratio;
  const float first = at - static_cast<float>(area.offset[along]) -
                      static_cast<float>(seam_width) / 2.0f;
  const float share = std::clamp(first / static_cast<float>(room), 0.0f, 1.0f);
  return std::round(share * ratio_steps) / ratio_steps;
}

// How long a bar of rows text cells, each cell long, is in an area whole long; none for
// no rows.
inline std::uint32_t
bar_length(std::uint32_t rows, std::uint32_t cell, std::uint32_t whole) {
  return rows == 0 ? 0 : std::min(rows * cell + 2 * bar_pad, whole);
}

} // namespace
