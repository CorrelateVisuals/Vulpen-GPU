// Where a pixel's ray met the nearest cube, as trace finds it, with what the nodes after
// it need of that cube there, so none of them looks it up again: 32 bytes, as a Ray.
struct Hit {
  vec4 position; // xyz: the point; w: how far along the ray, negative where it met none
  vec3 normal;   // the face's, unit
  uint look;     // the cube's color at the point, and as alpha how much it mirrors head
                 // on, 8 bits each, as packUnorm4x8 packs them
};

// How far off a face a ray leaving a hit starts, toward the side it met, so it never meets
// that face again for the rounding of where it is.
const float nudge = 1.0e-3;

VP_READS(Hits, Hit);
VP_WRITES(HitsOut, Hit);
