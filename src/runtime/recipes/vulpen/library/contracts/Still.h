#pragma once

namespace VP_VIEW {

// Whether a still holds a frame of the picture it copies, as space or a double click
// asked, which the image it hands the frame to shows in place of the picture going on.
struct Still {
  bool holds = false;
};

} // namespace VP_VIEW
