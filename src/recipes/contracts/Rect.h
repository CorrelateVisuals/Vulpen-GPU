#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstdint>

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

} // namespace VP_VIEW
