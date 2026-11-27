#pragma once

#include "runtime/Operator.h"

#include <vector>

namespace VP_VIEW {

// "He that cometh from above is above all: he that is of the earth is earthly, and
// speaketh of the earth: he that cometh from heaven is above all." John 3:31

// A ground a camera keeps over, as a terrain hands it on: heights on a grid, row by row,
// size.x points a row. The point at column i of row j lies at origin + i * across +
// j * along, raised by its height along the up of the camera that reads it; across and
// along lie square to that up. Between the points a reader samples bilinearly.
struct Ground {
  std::vector<float> heights;
  glm::uvec2 size{}; // points a row, and rows
  glm::vec3 origin{};
  glm::vec3 across{}; // from a point to the next in its row
  glm::vec3 along{};  // from a row to the next
};

} // namespace VP_VIEW
