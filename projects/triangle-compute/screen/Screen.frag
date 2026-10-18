#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Texture picture; // a connection: the image the raster stored into this frame
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 pixel;

// The image is the window's size, so a pixel shows its own texel.
void main() {
  pixel = sample_nearest(pass.picture, uv);
}
