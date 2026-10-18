#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Corner.glsl"

layout(set = 1, binding = 0) uniform Pass {
  uint frames_per_turn; // a param: frames a whole turn takes; 0 holds the triangle still
  Corners top;          // each a connection: one point's corner
  Corners right;
  Corners left;
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
  const Corner corners[3] = Corner[](pass.top.at[0], pass.right.at[0], pass.left.at[0]);
  // A vertex_count past 3 draws the triangle again, rather than reading past the array.
  const Corner corner = corners[gl_VertexIndex % corners.length()];
  const vec2 center =
      (corners[0].position.xy + corners[1].position.xy + corners[2].position.xy) / 3.0;
  const vec2 at = spin(corner.position.xy, center, angle_at(pass.frames_per_turn));
  gl_Position = vec4(square(at), corner.position.zw);
  color = corner.color.rgb;
}
