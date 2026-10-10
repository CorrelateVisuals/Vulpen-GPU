// The pass block both of the image's shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects place;     // a connection: where it shows, an instance a Rect
  Texture picture; // a connection: the image a draw renders, or one a node's C++ fills
} pass;
