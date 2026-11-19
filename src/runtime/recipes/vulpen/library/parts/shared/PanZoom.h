#pragma once

#include "runtime/Operator.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace {

// How far what an area shows moved, and how near it is: the graph's, and the Perform
// panel's image, so both move alike (RV09). A drag moves it as far as the pointer went,
// the wheel zooms it about the pointer, which stays over the same place, a drag with the
// middle button about where it was pressed, a step of the wheel each drag_step pixels it
// goes sideways, right nearer, and its home key brings it home; the part hands it the
// drags, turns and keys it takes.
struct PanZoom {
  static constexpr float step = 1.1f; // how much nearer a step of the wheel zooms
  static constexpr float drag_step = 20.0f; // pixels sideways a step of the wheel
  static constexpr float least = 0.25f;
  static constexpr float most = 4.0f;
  static constexpr std::string_view home_key = "h";

  glm::vec2 pan{}; // in pixels, from the area's top left
  float zoom = 1.0f;

  // Where it starts, unmoved and at zoom 1, as what it shows first fits its area.
  void home() {
    *this = {};
  }

  // Up, a positive turn, zooms in, about a point in pixels from the area's top left, as
  // far out as farthest, which the graph sets past least to go up a level there, and as
  // near as nearest, which an image sets past most to show its pixels.
  void wheel(float turn, glm::vec2 about, float farthest = least, float nearest = most) {
    const float to = std::clamp(zoom * std::pow(step, turn), farthest, nearest);
    pan = about - (about - pan) / zoom * to;
    zoom = to;
  }

  // Where a place of what it shows is, in pixels from the area's top left.
  glm::vec2 on_screen(glm::vec2 at) const {
    return pan + at * zoom;
  }

  // Where a picture of size pixels shows in an area of room pixels, from the area's top
  // left, as the image part's shaders draw it: letterboxed, as large as fits at its own
  // aspect and centered, then moved and zoomed. Its top left, and how far it reaches;
  // nothing for a picture of no size.
  std::pair<glm::vec2, glm::vec2> shows(glm::vec2 room, glm::vec2 size) const {
    if (size.x <= 0.0f || size.y <= 0.0f)
      return {};
    const glm::vec2 fit = size * std::min(room.x / size.x, room.y / size.y);
    return {on_screen((room - fit) * 0.5f), fit * zoom};
  }
};

} // namespace
