#version 460
#extension GL_GOOGLE_include_directive : require
#include "Image.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
  color = sample_linear(pass.picture, uv);
}
