// The pass block both of the image's shaders include, so the loader finds the same
// block in each.
#include "baseclasses/GpuLayout.glsl"
#include "contracts/Rect.glsl"

layout(set = 1, binding = 0) uniform Pass {
  Rects place;     // from its C++: the Rect its area gives, an instance; none while empty
  Texture picture; // a connection: an image a draw renders, a hosted view's window, or
                   // one a node's C++ fills; nothing while none reaches it
  vec2 pan;        // from its C++: how far a drag moved it, in pixels
  float zoom;      // and how near the wheel brought it
  Texture held;    // a connection: the frame a still holds of the picture, encoded as
                   // contracts/Still.glsl says; nothing while none reaches it
  uint holds;      // from its C++: 1 while the still holds it, which then shows instead
  uint loading;    // from its C++: 1 until the view whose window the picture is shows
                   // it, as its code waits for the live build; a loader turns instead
} pass;

// "Believe me that I am in the Father, and the Father in me: or else believe me for the
// very works' sake." John 14:11
