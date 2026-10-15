#pragma once

#include "runtime/Operator.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {

// How far what an area shows moved, and how near it is: the graph's, and the Perform
// panel's image, so both move alike (RV09). A drag moves it as far as the pointer went,
// the wheel zooms it about the pointer, which stays over the same place, and its home key
// brings it home; the part hands it the drags, turns and keys it takes.
struct PanZoom {
  static constexpr float step = 1.1f; // how much nearer a step of the wheel zooms
  static constexpr float least = 0.25f;
  static constexpr float most = 4.0f;
  static constexpr std::string_view home_key = "h";

  glm::vec2 pan{}; // in pixels, from the area's top left
  float zoom = 1.0f;

  // Where it starts, unmoved and at zoom 1, as what it shows first fits its area.
  void home() {
    *this = {};
  }

  // Up, a positive turn, zooms in, about a point in pixels from the area's top left.
  void wheel(float turn, glm::vec2 about) {
    const float to = std::clamp(zoom * std::pow(step, turn), least, most);
    pan = about - (about - pan) / zoom * to;
    zoom = to;
  }

  // Where a place of what it shows is, in pixels from the area's top left.
  glm::vec2 on_screen(glm::vec2 at) const {
    return pan + at * zoom;
  }
};

} // namespace
