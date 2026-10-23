#version 460
#extension GL_GOOGLE_include_directive : require
#include "Image.glsl"

layout(location = 0) in vec2 uv;
layout(location = 1) flat in float texel;
layout(location = 0) out vec4 color;

// Zoomed past a screen pixel a pixel of the image, each shows as a square of its own
// color, so a deep zoom shows what the view drew rather than a blur of it.
void main() {
  color = texel > 1.0 ? sample_nearest(pass.picture, uv) : sample_linear(pass.picture, uv);
}
