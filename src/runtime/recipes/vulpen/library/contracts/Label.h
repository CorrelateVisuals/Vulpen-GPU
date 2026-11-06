#pragma once

#include "runtime/Operator.h"

#include <array>
#include <cstdint>

namespace VP_VIEW {

// contracts/Label.glsl's Label and Character, member for member, as C++ writes them; the
// loader holds them to the shader's (RA03). std430 aligns a Label as its ivec2, so a list
// of them steps 40 bytes.
struct alignas(8) Label {
  glm::ivec2 offset{};
  glm::uvec2 extent{};
  std::uint32_t role = 0;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  float scale = 0.0f; // so a Label written without one draws at the font's size
  glm::uvec2 cut{};   // and its room starts at its offset

  static constexpr auto members() {
    return std::array{VP_MEMBER(Label, offset),
                      VP_MEMBER(Label, extent),
                      VP_MEMBER(Label, role),
                      VP_MEMBER(Label, first),
                      VP_MEMBER(Label, count),
                      VP_MEMBER(Label, scale),
                      VP_MEMBER(Label, cut)};
  }
};

struct Character {
  std::uint32_t code = 0;
  std::uint32_t label = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(Character, code), VP_MEMBER(Character, label)};
  }
};

} // namespace VP_VIEW
