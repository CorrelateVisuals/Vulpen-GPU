#version 460
#extension GL_GOOGLE_include_directive : require
#include "Draw.glsl"

void main() {
  pass.values.at[gl_VertexIndex] = 1.0;
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
