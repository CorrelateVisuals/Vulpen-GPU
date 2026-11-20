// A font as glyphs reads it: its cell, and how the atlas image lays its glyphs out,
// kept together so text is measured with the font it is drawn in. Every glyph has a cell
// of the widest one's width, on screen as in the atlas; where each starts its writer says
// (Character's x), as letters are as wide as they are drawn.
struct Font {
  uvec2 cell;   // a glyph's room: the widest one's width, and the height, in pixels
  uvec2 base;   // the cell at zoom 1, which a layout that keeps its size whatever the
                // zoom counts in, as the graph's boxes on its ground
  uvec2 atlas;  // the atlas image's width and height, in pixels
  uint columns; // of cells in a row of the atlas
  uint first;   // the code of its first glyph
  uint count;   // how many glyphs follow from it, row by row
};

VP_READS(Fonts, Font);
