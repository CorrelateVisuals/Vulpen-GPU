// The camera as orbit writes it and rays reads it: where the rays start and the frame
// they spread across. contracts/Camera.h is the same struct as C++ writes it.
struct Camera {
  vec4 eye;   // xyz: where every ray starts
  vec4 ahead; // xyz: toward the picture's middle, unit; w: the lens, how far the picture
              // is from the eye for a half height of 1, so the longer, the narrower
  vec4 right; // xyz: across the picture, unit
  vec4 up;    // xyz: up the picture, unit
};

VP_READS(Cameras, Camera);
