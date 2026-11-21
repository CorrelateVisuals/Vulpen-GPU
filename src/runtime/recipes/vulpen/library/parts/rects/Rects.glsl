// The pass block both of the rects' shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Palette.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects rects;     // a connection: what parts placed, an instance each
  Palette palette; // a connection: a color a role
} pass;

// "But to us there is but one God, the Father, of whom are all things, and we in him; and
// one Lord Jesus Christ, by whom are all things, and we by him." 1 Corinthians 8:6
