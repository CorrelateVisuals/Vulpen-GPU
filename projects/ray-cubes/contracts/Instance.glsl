// A cube as a ray meets it: the frame of one instance of the unit cube, which place works
// out once a frame from its Cube, so every ray tests it with dot products alone. count
// says how many there are, so no reader reads past the last (GLSL02).
struct Instance {
  vec4 center; // xyz: its middle
  vec4 x;      // xyz: its own x axis, unit; w: half its side along it
  vec4 y;      // likewise its y axis
  vec4 z;      // and its z axis
  vec4 inverse; // xyz: 1 over half its side along each axis, which scales a ray into it;
                // w: the radius of the sphere around it, which a ray that misses misses it
  vec4 color;  // rgb: its color; a: how much it mirrors, head on
  vec4 look;   // x: tiles across a face, 0 for none
};

VP_READS(Instances, Instance);
VP_WRITES(InstancesOut, Instance);
VP_READS(Count, uint);
VP_WRITES(CountOut, uint);
