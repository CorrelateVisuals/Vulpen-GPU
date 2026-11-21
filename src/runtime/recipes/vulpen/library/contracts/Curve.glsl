// A wire from one point to another, in pixels from the window's top left, and the palette
// role it is drawn in, along a cubic whose handles its writer gives, so it leaves and
// arrives as the writer needs: a connection leaves its writer heading right and reaches
// its reader from the left, and turns in a lane as tight as the lane is wide.
// Connections and relations are both curves, so one part draws every line the graph
// shows.

// "Every valley shall be filled, and every mountain and hill shall be brought low; and
// the crooked shall be made straight, and the rough ways shall be made smooth;" Luke 3:5

struct Curve {
  vec2 from;   // where it leaves
  vec2 to;     // where it arrives
  vec2 leave;  // its first handle, from from: the way it leaves, and how far it holds it
  vec2 arrive; // its second handle, back from to: the way it arrives
  uint role;   // as contracts/Palette.h names the roles
};

VP_READS(Curves, Curve);
