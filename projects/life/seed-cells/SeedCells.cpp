#include "runtime/Operator.h"

#include <cstdint>
#include <utility>

namespace {

// The cells are sown once, the frame after the node binds, which a load, a swap of this
// C++ and a change of its params all start, so another seed sows anew; the engine takes
// them only when it seeds, so every other frame the shader returns at once.
class SeedCells final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _sowing = node.value<std::uint32_t>("sowing");
    _sow = true;
  }

  void cook(VP::Cook &frame) override {
    frame.set(_sowing, std::uint32_t{std::exchange(_sow, false)});
  }

  VP::Value<std::uint32_t> _sowing;
  bool _sow = true;
};

} // namespace

VP_OPERATOR(SeedCells);
