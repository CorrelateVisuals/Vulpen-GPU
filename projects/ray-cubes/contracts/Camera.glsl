// The camera as the camera part writes it and rays reads it: where the rays start and
// the frame they spread across. contracts/Camera.h is the same struct as C++ writes it.
struct Camera {
  vec4 eye;   // xyz: where every ray starts
  vec4 ahead; // xyz: toward the picture's middle, unit; w: the lens, how far the picture
              // is from the eye for a half height of 1, so the longer, the narrower
  vec4 right; // xyz: across the picture, unit; w: near, where a draw's depth counts from
  vec4 up;    // xyz: up the picture, unit; w: far, where it counts to
};

VP_READS(Cameras, Camera);
