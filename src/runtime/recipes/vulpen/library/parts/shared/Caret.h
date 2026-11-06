#pragma once

#include "runtime/Operator.h"

#include <cstdint>

namespace {

// Where typing goes, marked alike in the editor and the terminal (RV09): a bar width
// pixels across at the start of the cell typed into, which blinks, as a person's editor's
// does. It shows whole as it comes into focus, as it moves and as a key reaches it, so it
// never hides while a person types, then hides and shows for blink frames each. It counts
// frames, as the frame block's time does, so a replay draws it alike (C01).
class Caret {
public:
  static constexpr std::uint32_t width = 4;
  // Frames it shows, then hides: half a second each at the run's default 60 a second.
  static constexpr std::uint64_t blink = 30;

  // Whether it shows this frame, where it is, asked every frame it is in focus: a frame
  // it was not asked in starts it whole again, as one out of focus or out of view.
  bool shows(std::uint64_t frame, glm::ivec2 at, bool typed) {
    if (typed || at != _at || frame != _asked + 1)
      _whole_from = frame;
    _at = at;
    _asked = frame;
    return (frame - _whole_from) / blink % 2 == 0;
  }

private:
  glm::ivec2 _at{};
  std::uint64_t _asked = 0;
  std::uint64_t _whole_from = 0;
};

} // namespace
