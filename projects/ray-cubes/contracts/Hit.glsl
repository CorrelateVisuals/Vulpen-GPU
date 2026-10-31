// Where a pixel's ray met the nearest cube, as trace finds it, with what the nodes after
// it need of that cube there, so none of them looks it up again.
struct Hit {
  vec4 position; // xyz: the point; w: how far along the ray, negative where it met none
  vec4 normal;   // xyz: the face's, unit; w: how much the cube mirrors, head on
  vec4 color;    // rgb: the cube's color at the point
};

// How far off a face a ray leaving a hit starts, toward the side it met, so it never meets
// that face again for the rounding of where it is.
const float nudge = 1.0e-3;

VP_READS(Hits, Hit);
VP_WRITES(HitsOut, Hit);
