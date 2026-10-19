#pragma once

#include "contracts/Label.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

// The Labels and their Characters a part writes for glyphs in a frame, filled in turn,
// so every part that shows text writes them alike (RV09): a Label's characters follow
// those written before it, and what the buffers lack is cut, a Label past their room
// written not at all, so no character lands outside them. The part hands it the buffers
// it writes, and sets their used lengths to its counts once it is done.
struct Labels {
  std::span<VP_VIEW::Label> labels;
  std::span<VP_VIEW::Character> characters;
  std::uint32_t label_count = 0;
  std::uint32_t character_count = 0;

  // A Label as the part places it, its offset, extent, role, scale and cut, holding as
  // much of the text as fits. Whether it was written.
  bool add(VP_VIEW::Label label, std::string_view text) {
    if (label_count == labels.size())
      return false;
    label.first = character_count;
    label.count = 0;
    labels[label_count++] = label;
    append(text);
    return true;
  }

  // More characters for the Label written last, as a run that goes on in its role.
  void append(std::string_view text) {
    VP_VIEW::Label &label = labels[label_count - 1];
    const auto fits = static_cast<std::uint32_t>(
        std::min(text.size(), characters.size() - character_count));
    for (std::uint32_t at = 0; at < fits; ++at)
      characters[character_count + at] = {.code = static_cast<unsigned char>(text[at]),
                                           .label = label_count - 1};
    character_count += fits;
    label.count += fits;
  }
};

// What of a name shows in so many cells, as a box's title or a tab cut to its room: all
// of it, or after ... the most of its end that fits from a / or a -, so a path keeps its
// last names, as .../triangle-params/triangle or ...-params/triangle; with none that
// fits, the shortest of them, and a name holding no / or -, whole, for the room to cut.
// Written into a string the part keeps, so a frame allocates nothing once it grew
// (CPP10).
inline void fit(std::string &into, std::string_view name, std::size_t cells) {
  constexpr std::string_view cut_mark = "...";
  constexpr std::string_view breaks = "/-";
  std::string_view end;
  for (std::size_t at = name.size() <= cells ? std::string_view::npos
                                             : name.find_first_of(breaks, 1);
       at != std::string_view::npos;
       at = name.find_first_of(breaks, at + 1)) {
    end = name.substr(at);
    if (cut_mark.size() + end.size() <= cells)
      break;
  }
  if (end.empty())
    into.assign(name);
  else
    into.assign(cut_mark).append(end);
}

} // namespace
