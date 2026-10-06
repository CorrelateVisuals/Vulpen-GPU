// The GPU layout, declared once for every shader. C++ never keeps a copy: it reads the
// layout back from SPIR-V reflection.
//
// A node's values and buffers live in the pass block its shader declares as
// `layout(set = 1, binding = 0) uniform Pass { ... } pass;`. The loader fills each field
// from the node's param of that name, from the node's C++, or, for a buffer, from a
// connection, and refuses a field that nothing fills (A02).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require

// Buffers are device addresses (RV02). The qualifier is the node's declaration of what
// it reads and writes; the engine places the barriers from it (V10).
layout(buffer_reference, std430) readonly buffer FloatsIn {
  float at[];
};
layout(buffer_reference, std430) writeonly buffer FloatsOut {
  float at[];
};
layout(buffer_reference, std430) buffer Floats {
  float at[];
};

// What every pass may read about the frame; the push constant holds its address (RV02).
// The engine writes each member by its name, at the offset reflection gives it (RA03).
layout(buffer_reference, std430) readonly buffer FrameBlock {
  uvec2 resolution; // of the window in pixels; zero without one
  vec2 cursor;      // in pixels, from the top left; zero until the input port feeds it
  float time;       // seconds, from the frame index at the run's rate, so a replay
                    // matches (C01); a float steps coarser than a frame after about 3 days
  uint index;       // the frame, as the node's C++ counts it; wraps after 2^32
};

layout(push_constant) uniform Push {
  FrameBlock frame;
};

// Every image a shader samples, and the static samplers (RV02). A pass block names an
// image by a Texture, its slot in textures[], which the loader fills from the node's C++
// or a connection. Slot 0 means unbound, and samples as nothing.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];

struct Texture {
  uint index;
};

// samplers[0] to samplers[3], as the engine makes them.
const uint linear_clamp = 0u;
const uint nearest_clamp = 1u;
const uint linear_repeat = 2u;
const uint nearest_repeat = 3u;

// At level 0, given, so no derivatives: every stage and every GPU samples alike
// (GLSL02). nonuniformEXT, since invocations may sample different images.
vec4 sample_linear(Texture image, vec2 uv) {
  if (image.index == 0u)
    return vec4(0.0);
  return textureLod(
      sampler2D(textures[nonuniformEXT(image.index)], samplers[linear_clamp]), uv, 0.0);
}
