#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Image canvas; // which a draw may not store into
} pass;

layout(location = 0) out vec4 color;

void main() {
  store(pass.canvas, ivec2(gl_FragCoord.xy), vec4(1.0));
  color = vec4(1.0);
}
