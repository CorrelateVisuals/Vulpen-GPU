#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstdint>

namespace VP_VIEW {

// contracts/Curve.glsl's Curve, member for member, as C++ writes it; the loader holds it
// to the shader's (RA03). std430 aligns it as its vec2, so a list of them steps 24 bytes.
struct alignas(8) Curve {
  glm::vec2 from{};
  glm::vec2 to{};
  std::uint32_t role = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Curve, from), VP_MEMBER(Curve, to), VP_MEMBER(Curve, role)};
  }
};

} // namespace VP_VIEW
