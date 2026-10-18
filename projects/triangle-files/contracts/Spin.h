#pragma once

namespace VP_VIEW {

// How far the triangle has turned, which the clock writes each frame and the corners read
// after it.
struct Spin {
  float angle = 0.0f; // radians
};

} // namespace VP_VIEW
