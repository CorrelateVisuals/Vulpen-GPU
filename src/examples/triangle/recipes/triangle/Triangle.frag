#version 460
#extension GL_GOOGLE_include_directive : require
#include "Triangle.glsl"

layout(location = 0) out vec4 color;

void main() {
  color = pass.tint;
}
