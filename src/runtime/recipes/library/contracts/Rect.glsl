// A rectangle in pixels, offset and extent as VkRect2D names them, and the palette role
// it is drawn in. The part that places a thing writes its Rect once, and drawing and hit
// testing read that same Rect, so what is drawn is what is clicked.
struct Rect {
  ivec2 offset; // its top left, in pixels from the window's top left
  uvec2 extent; // its width and height, in pixels
  uint role;    // as contracts/Palette.h names the roles
};

layout(buffer_reference, std430) readonly buffer Rects {
  Rect at[];
};
