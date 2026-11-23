#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  DrawCountsIn drawn;
} pass;

layout(location = 0) out vec4 color;

void main() {
  color = vec4(1.0);
}
