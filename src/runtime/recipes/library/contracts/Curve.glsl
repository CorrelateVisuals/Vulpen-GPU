// A wire from one point to another, in pixels from the window's top left, and the palette
// role it is drawn in. Connections and relations are both curves, so one part draws every
// line the graph shows.
struct Curve {
  vec2 from; // where it leaves, heading right
  vec2 to;   // where it arrives, from the left
  uint role; // as contracts/Palette.h names the roles
};

layout(buffer_reference, std430) readonly buffer Curves {
  Curve at[];
};
