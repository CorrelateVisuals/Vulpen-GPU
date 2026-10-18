// A corner of the triangle as the face draws it: where it is in clip space, y pointing
// down, and its color, which the rasterizer blends across the face toward the others'.
// contracts/Corner.h is the same struct as C++ writes it.
struct Corner {
  vec4 position;
  vec4 color;
};

layout(buffer_reference, std430) readonly buffer Corners {
  Corner at[];
};
