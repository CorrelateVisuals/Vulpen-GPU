#version 460
#extension GL_GOOGLE_include_directive : require
#include "Image.glsl"

layout(location = 0) out vec2 uv;

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
  if (pass.picture.index == 0u || rect.extent.x == 0u || rect.extent.y == 0u ||
      pass.zoom <= 0.0)
    return;
  const vec2 size = vec2(textureSize(
      sampler2D(textures[pass.picture.index], samplers[linear_clamp]), 0));
  const vec2 room = vec2(rect.extent);
  const vec2 shown = size * min(room.x / size.x, room.y / size.y);
  const vec2 from = pass.pan + (room - shown) * 0.5 * pass.zoom;
  const vec2 to = from + shown * pass.zoom;
  const vec2 kept = clamp(mix(from, to, quad_corner(gl_VertexIndex)), vec2(0.0), room);
  uv = (kept - from) / (to - from);
  gl_Position = pixel_clip(vec2(rect.offset) + kept);
}
