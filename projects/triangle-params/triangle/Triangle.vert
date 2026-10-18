#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"

// Every field a param of the node, which the engine writes at load and after each edit.
layout(set = 1, binding = 0) uniform Pass {
  uint frames_per_turn; // frames a whole turn takes; 0 holds the triangle still
  vec2 top;             // in clip space, y pointing down
  vec3 top_color;
  vec2 right;
  vec3 right_color;
  vec2 left;
  vec3 left_color;
} pass;

layout(location = 0) out vec3 color;

const float turn = 6.28318530718; // radians in a whole turn

// How far the triangle has turned this frame, counted from the frame index, so it never
// drifts.
float angle_at(uint frames_per_turn) {
  if (frames_per_turn == 0u)
    return 0.0;
  return turn * float(frame.index % frames_per_turn) / float(frames_per_turn);
}

// A point turned by the angle about the center.
vec2 spin(vec2 point, vec2 center, float angle) {
  const mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
  return center + rotation * (point - center);
}

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

void main() {
  const vec2 corners[3] = vec2[](pass.top, pass.right, pass.left);
  const vec3 colors[3] = vec3[](pass.top_color, pass.right_color, pass.left_color);
  // A vertex_count past 3 draws the triangle again, rather than reading past the arrays.
  const int corner = gl_VertexIndex % corners.length();
  const vec2 center = (corners[0] + corners[1] + corners[2]) / 3.0;
  const vec2 at = spin(corners[corner], center, angle_at(pass.frames_per_turn));
  gl_Position = vec4(square(at), 0.0, 1.0);
  color = colors[corner];
}
