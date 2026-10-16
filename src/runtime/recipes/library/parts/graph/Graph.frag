#version 460
#extension GL_GOOGLE_include_directive : require
#include "Graph.glsl"

layout(location = 0) flat in vec2 origin; // where the graph's top left shows
layout(location = 1) flat in vec4 ink;
layout(location = 0) out vec4 color;

// A line every spacing pixels of the graph, which the zoom scales, and every
// major_every-th a stronger one; each a pixel wide at any zoom, in its share of the ink.
const float spacing = 24.0;
const float major_every = 10.0;
const float minor_ink = 0.3;
const float major_ink = 0.6;
// Pixels apart on screen over which the lesser lines fade out as the graph zooms out, so
// the ground never crowds and no line jumps away.
const float fade_from = 4.0;
const float fade_to = 16.0;

// How much of a pixel the nearest line covers, of either axis, with lines apart pixels
// apart from the origin; a line on a whole pixel covers it alone.
float covered(vec2 at, float apart) {
  const vec2 away = abs(fract(at / apart + 0.5) - 0.5) * apart;
  const vec2 cover = clamp(1.0 - away, 0.0, 1.0);
  return max(cover.x, cover.y);
}

// From each pixel's top left, as a Rect counts, worked out from the zoom with no
// derivatives, so every GPU draws the same lines (GLSL02).
void main() {
  const vec2 at = gl_FragCoord.xy - 0.5 - origin;
  const float apart = spacing * pass.zoom;
  const float fade = clamp((apart - fade_from) / (fade_to - fade_from), 0.0, 1.0);
  const float minor = covered(at, apart) * minor_ink * fade;
  const float major = covered(at, apart * major_every) * major_ink;
  color = ink * max(minor, major);
}
