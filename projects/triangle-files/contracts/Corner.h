#pragma once

#include "runtime/Operator.h"

#include <array>

namespace VP_VIEW {

// contracts/Corner.glsl's Corner, member for member, as C++ writes it; the loader holds it
// to the shader's (RA03). Two vec4s, so std430 and C++ lay it out alike.
struct Corner {
  glm::vec4 position{};
  glm::vec4 color{};

  static constexpr auto members() {
    return std::array{VP_MEMBER(Corner, position), VP_MEMBER(Corner, color)};
  }
};

} // namespace VP_VIEW
