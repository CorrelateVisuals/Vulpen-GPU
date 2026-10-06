#include "runtime/Operator.h"

#include <array>
#include <format>
#include <span>
#include <stdexcept>

namespace {

using Kind = VP::Event::Kind;

// What fail-loud.py's input lines make, in their order, before the first frame.
std::array<VP::Event, 7> expected() {
  return {{{.kind = Kind::key, .down = true, .name = "a"},
           {.kind = Kind::text, .name = "hello world"},
           {.kind = Kind::pointer, .at = {12.5f, 40.0f}},
           {.kind = Kind::button, .down = true, .name = "left"},
           {.kind = Kind::wheel, .turn = {0.0f, -1.0f}},
           {.kind = Kind::focus},
           {.kind = Kind::key, .name = "enter"}}};
}

// Stops unless the first frame holds the events the input lines made, in order, and the
// next holds none, with the pointer where the last move left it.
class Input final : public VP::Operator {
  void cook(VP::Cook &frame) override {
    const std::span<const VP::Event> events = frame.input().events();
    const std::array<VP::Event, 7> made = expected();
    if (frame.index() == 0 && !std::ranges::equal(events, made))
      throw std::runtime_error(
          std::format("the first frame holds {} events, not the {} the input lines made",
                      events.size(),
                      made.size()));
    if (frame.index() == 1 && (!events.empty() || frame.input().pointer() != made[2].at))
      throw std::runtime_error("the events of the first frame did not stay in it");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Input>("Input");
}
