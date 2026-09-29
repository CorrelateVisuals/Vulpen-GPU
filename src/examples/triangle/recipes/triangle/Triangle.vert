#version 460
#extension GL_GOOGLE_include_directive : require
#include "Triangle.glsl"

void main() {
  gl_Position = vec4(pass.corners.at[gl_VertexIndex], 0.0, 1.0);
}
