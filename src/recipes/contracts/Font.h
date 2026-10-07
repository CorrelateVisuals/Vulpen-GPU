#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstdint>

namespace VP_VIEW {

// contracts/Font.glsl's Font, member for member, as C++ writes it; the loader holds it
// to the shader's (RA03). std430 aligns it as its uvec2, so a list of them steps 32 bytes.
struct alignas(8) Font {
  glm::uvec2 cell{};
  glm::uvec2 atlas{};
  std::uint32_t columns = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(Font, cell),
                      VP_MEMBER(Font, atlas),
                      VP_MEMBER(Font, columns),
                      VP_MEMBER(Font, first),
                      VP_MEMBER(Font, count)};
  }
};

} // namespace VP_VIEW
