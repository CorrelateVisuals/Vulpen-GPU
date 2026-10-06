#pragma once

#include <cstdint>

namespace VP_VIEW {

// What give hands take each frame: the frame it wrote it in.
struct Count {
  std::uint64_t frame = 0;
};

} // namespace VP_VIEW
