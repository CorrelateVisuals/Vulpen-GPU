// A rectangle in pixels, offset and extent as VkRect2D names them, and the palette role
// it is drawn in, or a blend of two. The part that places a thing writes its Rect once, and drawing and hit
// testing read that same Rect, so what is drawn is what is clicked.
struct Rect {
  ivec2 offset;    // its top left, in pixels from the window's top left
  uvec2 extent;    // its width and height, in pixels
  uint role;       // as contracts/Palette.h names the roles
  uint blend_role; // a second role, which its color blends toward
  float blend;     // how far: none at 0, all of it at 1
};

layout(buffer_reference, std430) readonly buffer Rects {
  Rect at[];
};
