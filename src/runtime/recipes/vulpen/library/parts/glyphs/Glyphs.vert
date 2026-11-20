#include "Glyphs.glsl"

layout(location = 0) out vec2 uv;
layout(location = 1) flat out vec4 ink;
layout(location = 2) flat out vec4 bounds; // its cell's first and last texel centers, in uv

// A character's place is its x along its label, which its writer measured with the
// font's advances, its cell the font's times the label's scale: x in atlas cells times the
// scaled cell, so a monospace column lands where it always has, bit for bit. Each corner
// is kept inside the label's room, its atlas coordinate moved as far, so a character half
// out of the room is cut at its edge, and one wholly out folds flat, without a discard
// (GLSL02). One outside its label's characters, or whose code the font lacks, folds to a
// point and draws nothing; so does every one until the font is written, whose zeros
// would divide below.
void main() {
  const Character character = pass.characters.at[gl_InstanceIndex];
  const Label label = pass.labels.at[character.label];
  const Font font = pass.font.at[0];
  const uint column = uint(gl_InstanceIndex) - label.first;
  const uint glyph = character.code - font.first;
  const vec2 atlas_cell = vec2(font.cell);
  const vec2 cell = atlas_cell * (label.scale == 0.0 ? 1.0 : label.scale);
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  uv = vec2(0.0);
  ink = vec4(0.0);
  bounds = vec4(0.0);
  if (column >= label.count || glyph >= font.count)
    return;
  const vec2 from = vec2(label.offset) + vec2(character.x / atlas_cell.x * cell.x, 0.0);
  // max, then min, as clamp is undefined for a room that ends before it starts (GLSL02).
  const vec2 kept = min(max(from + quad_corner(gl_VertexIndex) * cell,
                            vec2(label.offset) + vec2(label.cut)),
                        vec2(label.offset) + vec2(label.extent));
  gl_Position = pixel_clip(kept);
  const vec2 origin = vec2(glyph % font.columns, glyph / font.columns) * atlas_cell;
  uv = (origin + (kept - from) / cell * atlas_cell) / vec2(font.atlas);
  bounds = vec4(origin + 0.5, origin + atlas_cell - 0.5) / vec2(font.atlas).xyxy;
  ink = role_color(pass.palette, label.role);
}
