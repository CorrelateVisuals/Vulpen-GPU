// A corner of the triangle as a point writes it and the face draws it: where it is in clip
// space, y pointing down, and its color, which the rasterizer blends across the face
// toward the others'.
struct Corner {
  vec4 position;
  vec4 color;
};

VP_READS(Corners, Corner);
VP_WRITES(CornersOut, Corner);
