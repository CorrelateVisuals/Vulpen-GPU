// A corner of the triangle as the raster reads it: where it is in clip space, y pointing
// down, and its color, which the raster blends across the cells the triangle covers.
// contracts/Corner.h is the same struct as C++ writes it.
struct Corner {
  vec4 position;
  vec4 color;
};

VP_READS(Corners, Corner);
