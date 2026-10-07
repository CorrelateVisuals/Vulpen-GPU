// The pass block both of the glyphs' shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Font.glsl"
#include "contracts/Label.glsl"
#include "contracts/Palette.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Labels labels;         // a connection: the text parts show
  Characters characters; // a connection: their characters, an instance each
  Fonts font;            // a connection: the font's one Font
  Palette palette;       // a connection: a color a role
  Texture atlas;         // a connection: the font's glyphs
} pass;
