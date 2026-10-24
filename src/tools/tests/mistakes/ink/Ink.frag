#include "baseclasses/GpuLayout.glsl"

layout(location = 0) in vec2 uv;
layout(location = 1) in float shade;

layout(location = 0) out vec4 color;

// Each pixel's place, as paint draws it, from the varyings brush hands on: a pixel that
// reads back as another, or as nothing, shows they did not arrive.
void main() {
  const uvec2 pixel = uvec2(uv * vec2(frame.resolution));
  color = vec4(vec2(pixel & 255u) / 255.0 * shade, 0.0, 1.0);
}
