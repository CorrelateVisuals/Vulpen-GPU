#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Texture picture; // a connection: the image tone stored into this frame
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 pixel;

// The image is the frame's size, so a pixel shows its own texel; tone stored each as its
// square root, so squared it is the light again, which the window encodes as it shows it.
void main() {
  const vec3 stored = sample_nearest(pass.picture, uv).rgb;
  pixel = vec4(stored * stored, 1.0);
}
