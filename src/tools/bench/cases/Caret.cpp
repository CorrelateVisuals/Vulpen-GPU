#include "shared/Caret.h"

#include "tools/bench/Bench.h"

#include <cstdint>
#include <format>
#include <string>

// The library's shared/Caret.h: where typing goes, marked alike in the editor and the
// terminal, blinking by frames. A row is a run of frames a caret is asked in, | for each
// it shows and . for each it hides.

namespace VP::bench {

namespace {

// Frames from first to last, the caret at at, a key reaching it at typed, moved to moved
// at move, and not asked at skipped, as while out of focus.
std::string shown(std::uint64_t first,
                  std::uint64_t last,
                  std::uint64_t typed,
                  std::uint64_t move,
                  std::uint64_t skipped) {
  ::Caret caret;
  std::string marks;
  for (std::uint64_t frame = first; frame <= last; ++frame) {
    if (frame == skipped) {
      marks.push_back(' ');
      continue;
    }
    const glm::ivec2 at = frame >= move ? glm::ivec2(5, 1) : glm::ivec2(4, 1);
    marks.push_back(caret.shows(frame, at, frame == typed) ? '|' : '.');
  }
  return marks;
}

constexpr std::uint64_t never = ~std::uint64_t{0};

} // namespace

void cases(Bench &bench) {
  bench.sweep("Caret.h/Caret::shows", [](std::uint64_t frame, glm::ivec2 at, bool typed) {
    ::Caret caret;
    return caret.shows(frame, at, typed);
  });
  bench.check("Caret.h/Caret::shows", [](Rows &rows) {
    rows.row("still: " + shown(1, 130, never, never, never));
    rows.row("typed at 45: " + shown(1, 130, 45, never, never));
    rows.row("moved at 40: " + shown(1, 130, never, 40, never));
    rows.row("not asked at 50: " + shown(1, 130, never, never, 50));
    rows.row("from frame 0: " + shown(0, 70, never, never, never));
    rows.row("past the last frame: " + shown(never - 70, never - 1, never, never, never));
  });
  bench.holds("Caret.h/Caret::shows",
              "it shows whole on a frame a key reaches it",
              [](std::uint64_t frame, glm::ivec2 at) {
                ::Caret caret;
                caret.shows(frame - 1, at, false);
                return caret.shows(frame, at, true);
              });
}

} // namespace VP::bench
