#include "baseclasses/GpuLayout.glsl"
#include "contracts/Corner.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Corners corners; // a connection: the corners node's, one a vertex
} pass;

layout(location = 0) out vec3 color;

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

void main() {
  const Corner corner = pass.corners.at[gl_VertexIndex];
  gl_Position = vec4(square(corner.position.xy), corner.position.zw);
  color = corner.color.rgb;
}
