// A font as glyphs reads it: its cell, and how the atlas image lays its glyphs out,
// kept together so text is measured with the font it is drawn in. The font is
// monospace, so every glyph fills one cell, on screen as in the atlas.
struct Font {
  uvec2 cell;   // a glyph's width and height, in pixels
  uvec2 atlas;  // the atlas image's width and height, in pixels
  uint columns; // of cells in a row of the atlas
  uint first;   // the code of its first glyph
  uint count;   // how many glyphs follow from it, row by row
};

layout(buffer_reference, std430) readonly buffer Fonts {
  Font at[];
};
