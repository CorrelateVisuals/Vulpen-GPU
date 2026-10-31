// The triangle once, for C++ and GLSL alike: its corners and colors and the turn that
// moves the corners, written in what the two languages share, so the operator and the
// shader read one table and one function rather than a copy each (C02). C++ reads it
// through glm, whose names are GLSL's, and every float carries an f, which both accept.
#ifndef TRIANGLE_SHARED_H
#define TRIANGLE_SHARED_H

#ifdef __cplusplus
#include <glm/mat2x2.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace {

using glm::cos;
using glm::mat2;
using glm::sin;
using glm::vec2;
using glm::vec3;
using uint = std::uint32_t;
#endif

const float turn = 6.28318530718f; // radians in a whole turn

// The classic triangle: its corners in clip space, y pointing down, and a color each.
const vec2 corners[3] = {vec2(0.0f, -0.5f), vec2(0.5f, 0.5f), vec2(-0.5f, 0.5f)};
const vec3 colors[3] = {vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)};

// How far the triangle has turned at a frame, counted from the frame index, so it never
// drifts; none while frames_per_turn is 0.
float angle_at(uint frame_index, uint frames_per_turn) {
  if (frames_per_turn == 0u)
    return 0.0f;
  return turn * float(frame_index % frames_per_turn) / float(frames_per_turn);
}

// A corner turned by the angle about the middle of the three.
vec2 corner_at(int corner, float angle) {
  const vec2 middle = (corners[0] + corners[1] + corners[2]) / 3.0f;
  const mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
  return middle + rotation * (corners[corner] - middle);
}

#ifdef __cplusplus
} // namespace
#endif

#endif
