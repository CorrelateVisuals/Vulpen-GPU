#pragma once

#include "runtime/Operator.h"

#include <array>

namespace VP_VIEW {

// "For we have not followed cunningly devised fables, when we made known unto you the
// power and coming of our Lord Jesus Christ, but were eyewitnesses of his majesty."
// 2 Peter 1:16

// contracts/Camera.glsl's Camera, member for member, as arcball writes it; the loader
// holds it to the shader's (RA03). Four vec4s, so std430 and C++ lay it out alike.
struct Camera {
  glm::vec4 eye{};
  glm::vec4 ahead{};
  glm::vec4 right{};
  glm::vec4 up{};

  static constexpr auto members() {
    return std::array{VP_MEMBER(Camera, eye),
                      VP_MEMBER(Camera, ahead),
                      VP_MEMBER(Camera, right),
                      VP_MEMBER(Camera, up)};
  }
};

} // namespace VP_VIEW
