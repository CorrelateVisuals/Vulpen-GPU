#version 460
#extension GL_GOOGLE_include_directive : require
#include "Curves.glsl"

layout(location = 0) flat in vec4 fill;
layout(location = 1) flat in vec4 room;
layout(location = 0) out vec4 color;

// Nothing past the area's edge, where a pixel's center counts as a Rect counts a point;
// every draw blends premultiplied, so nothing there leaves what is drawn as it was, and
// no fragment is discarded.
void main() {
  const vec2 at = gl_FragCoord.xy;
  const bool inside = all(greaterThanEqual(at, room.xy)) && all(lessThan(at, room.zw));
  color = inside ? fill : vec4(0.0);
}
