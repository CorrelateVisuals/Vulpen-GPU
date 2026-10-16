#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstdint>
#include <vector>

namespace VP_VIEW {

// contracts/Rect.glsl's Rect, member for member, as C++ writes it; the loader holds it
// to the shader's (RA03). std430 aligns it as its ivec2, so a list of them steps 24 bytes.
struct alignas(8) Rect {
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  std::uint32_t role = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Rect, offset), VP_MEMBER(Rect, extent), VP_MEMBER(Rect, role)};
  }
};

// Rects one C++ node hands another, as a list hands hit the rows it shows.
using Rects = std::vector<Rect>;

// Whether a point, in pixels as the pointer counts them, is in the Rect. Its right and
// bottom edges are out, so of two Rects that touch only one holds a point between them.
inline bool contains(const Rect &rect, glm::vec2 point) {
  const glm::vec2 from(rect.offset);
  const glm::vec2 to = from + glm::vec2(rect.extent);
  return point.x >= from.x && point.x < to.x && point.y >= from.y && point.y < to.y;
}

} // namespace VP_VIEW
