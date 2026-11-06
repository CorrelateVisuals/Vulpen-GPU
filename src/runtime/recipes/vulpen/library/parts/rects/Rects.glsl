// The pass block both of the rects' shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Palette.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects rects;     // a connection: what parts placed, an instance each
  Palette palette; // a connection: a color a role
} pass;
