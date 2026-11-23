#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  DrawCountsIn drawn;
  DepthWrite depth;
} pass;

layout(location = 0) out vec4 color;

// Each pixel's place, as paint draws it, so look reads the image back pixel for pixel:
// with no instance drawn, it reads the cleared image instead.
void main() {
  const uvec2 pixel = uvec2(gl_FragCoord.xy);
  color = vec4(vec2(pixel & 255u) / 255.0, 0.0, 1.0);
}
