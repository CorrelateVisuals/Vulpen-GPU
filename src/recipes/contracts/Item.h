#pragma once

#include <string>
#include <vector>

namespace VP_VIEW {

// An entry a list shows, by its label, and the command a press on it sends, as a line
// typed at the command port. Completions, menus and tab strips are lists of Items, so
// none of them places its rows or finds the one pressed itself; the help a tooltip
// shows joins it once a part reads it.
struct Item {
  std::string label;
  std::string command; // empty for none
};

// What a part hands a list, in the order the list shows them.
using Items = std::vector<Item>;

} // namespace VP_VIEW
