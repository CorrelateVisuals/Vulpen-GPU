#include "baseclasses/GpuLayout.glsl"

// One quad over the whole window.
void main() {
  gl_Position = vec4(quad_corner(gl_VertexIndex) * 2.0 - 1.0, 0.0, 1.0);
}
