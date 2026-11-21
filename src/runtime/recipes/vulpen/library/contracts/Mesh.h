#pragma once

#include "runtime/Operator.h"

#include <array>

namespace VP_VIEW {

// "Behold my hands and my feet, that it is I myself: handle me, and see; for a spirit
// hath not flesh and bones, as ye see me have." Luke 24:39

// contracts/Mesh.glsl's Triangle, member for member, as mesh-file writes it; the loader
// holds it to the shader's (RA03).
struct Triangle {
  glm::vec4 a{};
  glm::vec4 b{};
  glm::vec4 c{};
  glm::vec4 normal_a{};
  glm::vec4 normal_b{};
  glm::vec4 normal_c{};
  glm::vec4 color{};

  static constexpr auto members() {
    return std::array{VP_MEMBER(Triangle, a),
                      VP_MEMBER(Triangle, b),
                      VP_MEMBER(Triangle, c),
                      VP_MEMBER(Triangle, normal_a),
                      VP_MEMBER(Triangle, normal_b),
                      VP_MEMBER(Triangle, normal_c),
                      VP_MEMBER(Triangle, color)};
  }
};

} // namespace VP_VIEW
