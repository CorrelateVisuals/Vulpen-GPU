// The GPU layout, declared once for every shader. C++ never keeps a copy: it reads the
// layout back from SPIR-V reflection.
//
// A node's values and buffers live in the pass block its shader declares as
// `layout(set = 1, binding = 0) uniform Pass { ... } pass;`. The loader fills each field
// from the node's param of that name, from the node's C++, or, for a buffer, from a
// connection, and refuses a field that nothing fills (A02).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require

// Buffers are device addresses (RV02), each declared in one line by its name and what
// an element is, as VP_READS(Corners, Corner); a shader reaches its elements as at[].
// The qualifier in the macro's name is the node's declaration of what it reads and
// writes; the engine places the barriers from it (V10).
#define VP_READS(Name, Element)                                                          \
  layout(buffer_reference, std430) readonly buffer Name {                                \
    Element at[];                                                                        \
  }
#define VP_WRITES(Name, Element)                                                         \
  layout(buffer_reference, std430) writeonly buffer Name {                               \
    Element at[];                                                                        \
  }
#define VP_BUFFER(Name, Element)                                                         \
  layout(buffer_reference, std430) buffer Name {                                         \
    Element at[];                                                                        \
  }

VP_READS(FloatsIn, float);
VP_WRITES(FloatsOut, float);
VP_BUFFER(Floats, float);

// What every pass may read about the frame; the push constant holds its address (RV02).
// The engine writes each member by its name, at the offset reflection gives it (RA03).
layout(buffer_reference, std430) readonly buffer FrameBlock {
  uvec2 resolution; // of the window in pixels, or without one --size's; else zero
  vec2 cursor;      // the pointer, in pixels from the top left; zero until it moves
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

// The texel nearest uv, for an image read one pixel to one texel.
vec4 sample_nearest(Texture image, vec2 uv) {
  if (image.index == 0u)
    return vec4(0.0);
  return textureLod(
      sampler2D(textures[nonuniformEXT(image.index)], samplers[nearest_clamp]), uv, 0.0);
}

// Between the image's mip levels, level 0 the image and each next half the one before,
// as C++ uploaded them (mip_levels): for an image drawn smaller than it is, at the
// log2 of its texels a pixel. Given, so no derivatives, as above; past the last level the
// last.
vec4 sample_level(Texture image, vec2 uv, float level) {
  if (image.index == 0u)
    return vec4(0.0);
  return textureLod(
      sampler2D(textures[nonuniformEXT(image.index)], samplers[linear_clamp]), uv, level);
}

// A draw whose pass block holds one of these tests each fragment against the depth the
// draws before it left in its target this frame, keeping it where it lies as near or
// nearer: gl_Position's z / w, 0 near and 1 far, which each frame clears to 1. A
// DepthWrite then leaves its own depth there, a DepthTest only tests, as a surface seen
// through does. A view's window has a depth for its draws to share; an image a draw
// renders into has one while that draw declares it. A draw without one stacks over what
// came before, in graph order. Its index is 0: the declaration is all a draw needs. Its
// name is a port too: a connection from it to a later pass's Texture hands that pass the
// depth of the image the draw renders into, which it reads by sample_nearest, as some
// GPUs cannot filter a depth.
struct DepthTest {
  uint index;
};

struct DepthWrite {
  uint index;
};

// Every image a dispatch stores into (RV02), each also in textures[] at the same slot, so
// a pass after it samples it through a Texture of that index. A pass block names one by
// an Image, which the engine makes at the frame's size, zeroed. One format,
// R8G8B8A8_UNORM, as rgba8 declares, so a store needs no feature past the floor's but
// storage image dynamic indexing.
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D images[];

struct Image {
  uint index;
};

// The image's size in pixels; zero for an unbound one, slot 0. An Image comes from the
// pass block, the same for every invocation, as indexing images[] without nonuniformEXT
// asks.
ivec2 image_size(Image image) {
  if (image.index == 0u)
    return ivec2(0);
  return imageSize(images[image.index]);
}

// color at pixel, from the top left; nothing into an unbound image, and nothing outside
// it, which no GPU then writes differently (GLSL02).
void store(Image image, ivec2 pixel, vec4 color) {
  const ivec2 size = image_size(image);
  if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size)))
    return;
  imageStore(images[image.index], pixel, color);
}

// The two triangles of a quad, as its corners from the top left, so a draw of six
// vertices an instance places a rectangle an instance.
const int quad_vertices = 6;
const vec2 quad_corners[quad_vertices] = vec2[quad_vertices](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

vec2 quad_corner(int vertex) {
  return quad_corners[vertex % quad_vertices];
}

// A point in pixels from the window's top left, where clip space puts it. Only a draw
// asks, and a draw runs only into a window or an image of the frame's size, so its
// resolution is never zero here.
vec4 pixel_clip(vec2 pixels) {
  return vec4(pixels / vec2(frame.resolution) * 2.0 - 1.0, 0.0, 1.0);
}
