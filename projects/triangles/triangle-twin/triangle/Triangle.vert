#include "baseclasses/GpuLayout.glsl"
#include "shared/Triangle.h"

VP_READS(Points, vec2);

layout(set = 1, binding = 0) uniform Pass {
  Points points; // corner_at's, from the Triangle operator every frame, one a vertex
} pass;

layout(location = 0) out vec3 color;

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

void main() {
  gl_Position = vec4(square(pass.points.at[gl_VertexIndex]), 0.0, 1.0);
  color = colors[gl_VertexIndex % colors.length()];
}
