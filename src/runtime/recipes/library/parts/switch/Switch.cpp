#include "contracts/Rect.h"
#include "runtime/Operator.h"

#include <array>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 2> halves{"first", "second"};

// Hands the Rect its area gives to one of two halves, the one its shown param names, and
// none to the other, which does no work, as a panel shows one of the contents it holds;
// the param is set by a line, so a save keeps it and a replay repeats it (V08).
class Switch final : public VP::Operator {
  void bind(VP::Bind &node) override {
    const std::string shown = node.param<std::string>("shown");
    if (shown != halves[0] && shown != halves[1])
      throw std::runtime_error(std::format("param shown = {}: first or second", shown));
    _shown = shown == halves[0] ? 0 : 1;
    _area = &node.input<VP_VIEW::Rect>("area");
    _halves = {&node.output<VP_VIEW::Rect>(halves[0]),
               &node.output<VP_VIEW::Rect>(halves[1])};
  }

  void cook(VP::Cook &) override {
    *_halves[_shown] = *_area;
    *_halves[1 - _shown] = {};
  }

  std::size_t _shown = 0;
  const VP_VIEW::Rect *_area = nullptr;
  std::array<VP_VIEW::Rect *, 2> _halves{};
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Switch>("Switch");
}
