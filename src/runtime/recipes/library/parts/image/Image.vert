#include "Image.glsl"

layout(location = 0) out vec2 uv;
layout(location = 1) flat out float texel; // screen pixels a pixel of the image covers

// What it shows: the frame held while one is, else the picture. Made from the index, as
// the pass block's Texture is laid out as the block is, and a local one is not.
Texture showing() {
  return Texture(pass.holds != 0u ? pass.held.index : pass.picture.index);
}

// Letterboxed: as large as fits in the Rect at the image's own aspect, and centered, so
// what is behind the Rect shows beside it; then moved and zoomed about the Rect's top
// left, as its C++ says. Each corner is kept inside the Rect, its image coordinate moved
// as far, so a zoomed image is cut at the Rect's edge without a discard (GLSL02). With no
// image yet, no room or no zoom, it folds to a point and draws nothing, since the zeros
// would divide below.
void main() {
  const Rect rect = pass.place.at[gl_InstanceIndex];
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  uv = vec2(0.0);
  texel = 0.0;
  const Texture image = showing();
  if (image.index == 0u || rect.extent.x == 0u || rect.extent.y == 0u || pass.zoom <= 0.0)
    return;
  const vec2 size =
      vec2(textureSize(sampler2D(textures[image.index], samplers[linear_clamp]), 0));
  const vec2 room = vec2(rect.extent);
  const vec2 shown = size * min(room.x / size.x, room.y / size.y);
  const vec2 from = pass.pan + (room - shown) * 0.5 * pass.zoom;
  const vec2 to = from + shown * pass.zoom;
  const vec2 kept = clamp(mix(from, to, quad_corner(gl_VertexIndex)), vec2(0.0), room);
  uv = (kept - from) / (to - from);
  texel = shown.x * pass.zoom / size.x;
  gl_Position = pixel_clip(vec2(rect.offset) + kept);
}
