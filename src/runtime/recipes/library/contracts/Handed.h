#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VP_VIEW {

// Lines a part hands a command line, as a menu's items do, so a press teaches the line it
// sends (V06). Counted, so the command line takes each batch once, in whichever frame it
// reads them.
struct Handed {
  // Run there as if typed and sent, its rows and its history showing it as a person's,
  // with no key pressed; begun, typed up to the word a placeholder wants, for the person
  // to finish there; or begun and the words that may come there listed, as Tab lists
  // them, as for a list too long for a menu's column.
  enum class Way : std::uint8_t { run, begin, list };
  struct Line {
    std::string text;
    Way way = Way::run;
  };
  std::uint64_t count = 0; // batches handed so far, the last of them below
  std::vector<Line> lines;
};

} // namespace VP_VIEW
