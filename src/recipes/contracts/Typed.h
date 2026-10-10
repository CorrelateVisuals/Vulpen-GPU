#pragma once

#include "runtime/Operator.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP_VIEW {

// The keys and text the keys part hands on, and the node they go to: the one in focus.
// A part that takes keys reads them here, not from the input port, so a key reaches one
// part, and one a chord turned into a command reaches none. The focus changes only as the
// keys part cooks, so every part reading a frame's keys agrees on whose they are.
struct Typed {
  std::string focus; // the node's name, as a line names it
  std::vector<VP::Event> events;

  // All of them for the node in focus, none for any other.
  std::span<const VP::Event> to(std::string_view node) const {
    return node == focus ? std::span(events) : std::span<const VP::Event>();
  }
};

} // namespace VP_VIEW
