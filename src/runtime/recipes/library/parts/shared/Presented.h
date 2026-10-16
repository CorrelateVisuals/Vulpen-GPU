#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <string_view>

namespace {

// The view presented: the one a view hosts whose window a connection of that view takes,
// the one hosted most recently of them when several are; empty for none. The parts that
// work on one view of many follow it, else the view hosted most recently, so all of them
// work on the view the window shows (RV09).
inline std::string_view presented(const VP::View &view) {
  std::string_view shown;
  for (const VP::Child &child : view.children)
    if (std::ranges::any_of(view.connections, [&](const VP::Connection &connection) {
          return connection.from.view() && connection.from.node == child.name;
        }))
      shown = child.name;
  return shown;
}

} // namespace
