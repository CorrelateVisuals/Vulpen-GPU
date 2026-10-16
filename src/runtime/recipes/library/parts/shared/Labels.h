#pragma once

#include "contracts/Label.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
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

} // namespace
