#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VP_VIEW {

// An entry a list shows, by its label, and the command a press on it sends, as a line
// typed at the command port. Completions, menus and tab strips are lists of Items, so
// none of them places its rows or finds the one pressed itself; the help a tooltip
// shows joins it once a part reads it. In a strip, an Item with no label is a divider,
// a line between the Items beside it, which no press reaches.
struct Item {
  std::string label;
  std::string command;  // empty for none
  bool current = false; // the one chosen, as the tab whose content shows
  // What a press on its close mark sends, as a tab's content closes what the tab shows;
  // empty for no mark.
  std::string close;
  // What a drag of it onto another Item of its list sends, that Item's move's last word
  // after it, as a tab moves to another's place among those of its content: a line whose
  // last word names it, as tab move <file>; empty for none, which no drag moves.
  std::string move;
  // What a right press on it sends, as a tab opens a menu of what to do with it; empty
  // for none.
  std::string menu;
  // One of the Items beside it of which one is on, as edit and run: a strip joins them,
  // the one on filled in the accent's color.
  bool toggles = false;
  // The role of the theme it is lit in: one that toggles is filled in it while on, as the
  // gpu button in the build curves' orange; any other shows its label in it while current
  // and muted while not, on a button's plain fill, as a layer's button in its curves'
  // color. None for the accent's of one that toggles, or the content's of a tab.
  std::optional<std::uint32_t> role;

  bool operator==(const Item &) const = default;
};

// What a part hands a list, in the order the list shows them.
using Items = std::vector<Item>;

} // namespace VP_VIEW
