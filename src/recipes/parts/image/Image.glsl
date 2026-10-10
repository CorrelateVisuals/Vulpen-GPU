// The pass block both of the image's shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects place;     // from its C++: the Rect its area gives, an instance; none while empty
  Texture picture; // a connection: an image a draw renders, a hosted view's window, or
                   // one a node's C++ fills; nothing while none reaches it
} pass;
