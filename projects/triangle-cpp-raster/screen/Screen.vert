#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Cell.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Cells cells; // a connection: the raster's, an instance each, as instance_count counts
} pass;

layout(location = 0) flat out vec4 color;

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

// Each cell a square of two triangles, so the grid shows as the cells it holds.
void main() {
  const Cell cell = pass.cells.at[gl_InstanceIndex];
  const vec2 at = cell.corner + quad_corner(gl_VertexIndex) * cell.size;
  gl_Position = vec4(square(at), 0.0, 1.0);
  color = cell.color;
}
