// A cell of the grid as seed-cells sows it, the engine steps it and cells draws it:
// Generations' Cell, without its goods, its terrain and its time.
struct Cell {
  vec4 position; // xy: its square's top left in clip space, y pointing down; w: its size
  vec4 color;
  ivec4 states; // x: alive or dead
};

const int alive = 1;
const int dead = -1;

VP_READS(Cells, Cell);
VP_WRITES(CellsOut, Cell);
