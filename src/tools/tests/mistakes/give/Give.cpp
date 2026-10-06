#include "contracts/Count.h"
#include "runtime/Operator.h"

namespace {

// A type only this file can name.
struct Hidden {
  int value = 0;
};

// Hands take the frame it cooks in, through a connection between C++ nodes.
class Give final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _count = &node.output<VP_VIEW::Count>("count");
  }
  void cook(VP::Cook &frame) override {
    _count->frame = frame.index();
  }

  VP_VIEW::Count *_count = nullptr;
};

// Outputs a type no reader can name, which the loader must refuse.
class Secret final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.output<Hidden>("count");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Give>("Give");
  registry.add<Secret>("Secret");
}
