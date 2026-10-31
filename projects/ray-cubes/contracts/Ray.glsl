// A ray a pixel: where it starts, where it goes, and how much of the light it finds
// reaches the eye, as rays casts it and each bounce's reflect sends it on.
struct Ray {
  vec4 origin;    // xyz
  vec4 direction; // xyz: unit
  vec4 weight;    // rgb: what reaches the eye of the light it finds; a: 1 while it goes
                  // on, 0 once it ended
};

VP_READS(Rays, Ray);
VP_WRITES(RaysOut, Ray);
