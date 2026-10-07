#pragma once

#include <string>
#include <vector>

namespace VP_VIEW {

// An entry a list shows, by its label. Completions, menus and tab strips are lists of
// Items, so none of them places its rows itself; the command a press sends and the help
// a tooltip shows join it once a part reads them.
struct Item {
  std::string label;
};

// What a part hands a list, in the order the list shows them.
using Items = std::vector<Item>;

} // namespace VP_VIEW
