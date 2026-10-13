#version 460
#extension GL_GOOGLE_include_directive : require
#include "Glyphs.glsl"

layout(location = 0) in vec2 uv;
layout(location = 1) flat in vec4 ink;
layout(location = 0) out vec4 color;

// The atlas holds each glyph's coverage, which scales the premultiplied ink. A cell at the
// font's size lies on whole pixels, each on a texel's center, so filtering changes it
// not at all; a cell scaled blends the texels it falls between.
void main() {
  color = ink * sample_linear(pass.atlas, uv).r;
}
