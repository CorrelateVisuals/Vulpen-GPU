// The pass block both of the graph's shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Palette.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects ground;    // from its C++: its area, in the role of the ground's lines, then
                   // the frame's inside, in the role it fills, an instance each; none
                   // while it shows no graph. A connection takes it to the curves, to be
                   // cut at its first
  vec2 pan;        // from its C++: how far the graph moved, in pixels
  float zoom;      // and how near it is
  Palette palette; // a connection: a color a role
} pass;
