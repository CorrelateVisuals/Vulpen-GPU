#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VP_VIEW {

// Lines a part hands a command line, as a menu's items do, so a press teaches the line it
// sends (V06): each run there as if typed and sent, its rows and its history showing it as
// a person's, with no key pressed, or begun, typed up to the word a placeholder wants, for
// the person to finish there. Counted, so the command line takes each batch once, in
// whichever frame it reads them.
struct Handed {
  struct Line {
    std::string text;
    bool run = true; // or begun
  };
  std::uint64_t count = 0; // batches handed so far, the last of them below
  std::vector<Line> lines;
};

} // namespace VP_VIEW
