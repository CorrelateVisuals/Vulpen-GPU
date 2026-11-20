#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  FloatsIn angles; // the wire from corners: a corner's angle an element, in radians
} pass;

layout(location = 0) out vec3 color;

const uint corners = 3u;
const float radius = 0.6; // of the circle the corners stand on, in clip space
const vec3 colors[corners] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                                    vec3(0.0, 0.0, 1.0));

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

void main() {
  // A vertex_count past 3 draws the triangle again, rather than reading past the wire.
  const uint corner = gl_VertexIndex % corners;
  const float angle = pass.angles.at[corner];
  gl_Position = vec4(square(radius * vec2(cos(angle), sin(angle))), 0.0, 1.0);
  color = colors[corner];
}
