#version 460
#extension GL_GOOGLE_include_directive : require
#include "Name.glsl"

// layout(location = 0) out vec2 uv; // to the fragment shader

void main() {
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  // gl_Position = vec4(pass.points.at[gl_VertexIndex] * pass.scale, 0.0, 1.0);
  // const uint instance = gl_InstanceIndex;
}
