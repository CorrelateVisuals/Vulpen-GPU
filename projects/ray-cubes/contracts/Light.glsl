// The one light, as lamp writes it, which every shadow and shade node reads.
struct Light {
  vec4 position; // xyz: where it is
  vec4 color;    // rgb: its color times its strength
};

VP_READS(Lights, Light);
VP_WRITES(LightsOut, Light);
