// The GPU layout, declared once for every shader. C++ never keeps a copy: it reads the
// layout back from SPIR-V reflection.
//
// A node's values and buffers live in the pass block its shader declares as
// `layout(set = 1, binding = 0) uniform Pass { ... } pass;`. The loader fills each field
// from the node's param of that name, from the node's C++, or, for a buffer, from a
// connection, and refuses a field that nothing fills (A02).
#extension GL_EXT_buffer_reference : require

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
