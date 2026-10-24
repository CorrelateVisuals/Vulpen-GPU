// A square of the grid a raster fills, as the screen draws it: its color, premultiplied,
// none at all outside the triangle; its top left corner in clip space, y pointing down;
// and its side. Each cell carries its own place, so the screen needs no grid of its own.
struct Cell {
  vec4 color;
  vec2 corner;
  float size;
};

VP_READS(Cells, Cell);
VP_WRITES(CellsOut, Cell);
