// A triangle of a mesh, as mesh-file writes it from a model's file and mesh draws it:
// its corners, the surface's direction at each, and its color. contracts/Mesh.h is the
// same struct as C++ writes it. Each a vec4, so std430 and C++ lay it out alike.
struct Triangle {
  vec4 a;        // xyz: a corner; w: 1
  vec4 b;        // xyz: the next corner, counterclockwise seen from the front
  vec4 c;        // xyz: the last corner
  vec4 normal_a; // xyz: the surface's direction at a, unit; w: 0
  vec4 normal_b;
  vec4 normal_c;
  vec4 color;    // rgb: the corners' colors averaged, white where the file gives none
};

VP_READS(Triangles, Triangle);
