#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  uint frames_per_turn; // a param: frames a whole turn takes; 0 holds the triangle still
} pass;

layout(location = 0) out vec3 color;

const float turn = 6.28318530718; // radians in a whole turn

// The classic triangle: its corners in clip space, y pointing down, and a color each.
const vec2 corners[3] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
const vec3 colors[3] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));

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
  // A vertex_count past 3 draws the triangle again, rather than reading past the arrays.
  const int corner = gl_VertexIndex % corners.length();
  const vec2 center = (corners[0] + corners[1] + corners[2]) / 3.0;
  const vec2 at = spin(corners[corner], center, angle_at(pass.frames_per_turn));
  gl_Position = vec4(square(at), 0.0, 1.0);
  color = colors[corner];
}
