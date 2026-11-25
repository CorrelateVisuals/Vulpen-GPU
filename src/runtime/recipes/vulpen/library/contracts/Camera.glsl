// The camera as the camera part writes it and a shader reads it: where the rays start
// and the frame they spread across, as ray-cubes' rays cast them, or the mesh part
// projects its corners. contracts/Camera.h is the same struct as C++ writes it.

// "His head and his hairs were white like wool, as white as snow; and his eyes were as a
// flame of fire;" Revelation 1:14

struct Camera {
  vec4 eye;   // xyz: where every ray starts
  vec4 ahead; // xyz: toward the picture's middle, unit; w: the lens, how far the picture
              // is from the eye for a half height of 1, so the longer, the narrower
  vec4 right; // xyz: across the picture, unit; w: near, where a draw's depth counts from
  vec4 up;    // xyz: up the picture, unit; w: far, where it counts to
};

VP_READS(Cameras, Camera);
