// The pass block both of the curves' shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Curve.glsl"
#include "contracts/Palette.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Curves curves;   // a connection: the lines a part drew between points, an instance each
  Palette palette; // a connection: a color a role
} pass;

// A curve is a strip of quads along its length, as many as view.vlp's vertex_count holds.
const int segments = 16;
const float half_width = 1.0; // in pixels
