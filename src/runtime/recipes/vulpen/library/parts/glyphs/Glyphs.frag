#include "Glyphs.glsl"

layout(location = 0) in vec2 uv;
layout(location = 1) flat in vec4 ink;
layout(location = 2) flat in vec4 bounds;
layout(location = 0) out vec4 color;

// The atlas holds each glyph's coverage, which scales the premultiplied ink. A cell at the
// font's size lies on whole pixels, each on a texel's center, so filtering changes it
// not at all; a cell scaled blends the texels it falls between, but never past its own
// cell's edge texels, so no neighbor's ink shows at its edge. min then max, as clamp is
// undefined for bounds that cross (GLSL02).
void main() {
  color = ink * sample_linear(pass.atlas, max(min(uv, bounds.zw), bounds.xy)).r;
}
