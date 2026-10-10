#version 460
#extension GL_GOOGLE_include_directive : require
#include "baseclasses/GpuLayout.glsl"

layout(location = 0) out vec4 color;

// Blocks of 8 pixels, red where the block's column and the frame add up odd, green where
// its row is odd: 0 and 1 only, which an sRGB image holds exactly. Red flips every frame,
// so a reader that samples the frame before finds the other color.
void main() {
  const uvec2 block = uvec2(gl_FragCoord.xy) >> 3;
  color = vec4(float((block.x + frame.index) & 1u), float(block.y & 1u), 0.0, 1.0);
}
