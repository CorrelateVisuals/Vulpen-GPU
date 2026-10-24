#include "baseclasses/GpuLayout.glsl"
#include "contracts/Cell.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Cells cells; // a connection: the engine's, an instance each, as instance_count counts
} pass;

layout(location = 0) out vec4 frag_color;

const float gap = 0.1; // of a cell's side, left open toward the next

// Square in the middle of any window: its shorter side spans -1 to 1.
vec2 square(vec2 at) {
  const vec2 size = vec2(frame.resolution);
  return at * min(size.yx / size, vec2(1.0));
}

// Each cell alive a square of two triangles; a dead one goes outside clip space, so it
// draws nothing.
void main() {
  uint cell_index = gl_InstanceIndex;
  Cell c = pass.cells.at[cell_index];
  vec4 in_position = c.position;
  vec4 in_color = c.color;
  ivec4 in_states = c.states;

  bool alive_cell = in_states.x == alive;
  if (!alive_cell) {
    frag_color = vec4(0.0);
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }

  vec2 corner = (0.5 * gap + (1.0 - gap) * quad_corner(gl_VertexIndex)) * in_position.w;
  gl_Position = vec4(square(in_position.xy + corner), 0.0, 1.0);
  frag_color = in_color;
}
