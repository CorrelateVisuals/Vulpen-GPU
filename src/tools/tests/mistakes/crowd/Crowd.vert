#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  DrawCountsIn drawn; // its instance count, which count's invocations add up to
  DepthWrite depth;   // the nearest instance's depth, which seen samples
} pass;

// Each instance one triangle over the whole image, each later one nearer, so the depth
// holds the last one's.
void main() {
  const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  const float near = 1.0 - float(gl_InstanceIndex + 1) / 8.0;
  gl_Position = vec4(corner * 2.0 - 1.0, near, 1.0);
}
