// A cube as grid lays it out, spin moves it and floor flattens one into the floor: a
// unit cube, scaled, turned and placed, which place turns into an Instance.
struct Cube {
  vec4 center; // xyz: its middle
  vec4 size;   // xyz: half its side along each of its own axes
  vec4 turn;   // xyz: the axis it is turned about, unit; w: how far, in radians
  vec4 color;  // rgb: its color; a: how much of what meets it, head on, it mirrors
  vec4 motion; // x: its turns a second; y: how high it bobs; z: where in its bob it starts
  vec4 look;   // x: tiles across a face, a checker of its color and a darker one; 0 none
};

VP_READS(Cubes, Cube);
VP_WRITES(CubesOut, Cube);
