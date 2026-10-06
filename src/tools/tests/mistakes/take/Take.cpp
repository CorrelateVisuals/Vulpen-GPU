#include "contracts/Count.h"
#include "runtime/Operator.h"

#include <format>
#include <stdexcept>
#include <vector>

namespace {

// Stops when what give handed it was not written this frame, as a writer cooks first.
class Take final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _count = &node.input<VP_VIEW::Count>("count");
  }
  void cook(VP::Cook &frame) override {
    if (_count->frame != frame.index())
      throw std::runtime_error(std::format(
          "give handed it frame {} in frame {}", _count->frame, frame.index()));
  }

  const VP_VIEW::Count *_count = nullptr;
};

// Reads the count as another type, which the loader must refuse.
class Mistyped final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.input<std::vector<int>>("count");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Take>("Take");
  registry.add<Mistyped>("Mistyped");
}
