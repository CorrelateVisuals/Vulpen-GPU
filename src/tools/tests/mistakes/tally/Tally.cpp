#include "runtime/Operator.h"

#include <array>
#include <cstdint>

namespace {

// The shader's DrawCount, as GpuLayout.glsl declares it; the loader holds both to it.
struct DrawCount {
  std::uint32_t instance_count = 0;

  static constexpr auto members() {
    return std::array{VP_MEMBER(DrawCount, instance_count)};
  }
};

// Writes its own draw's DrawCount, which the frame zeroes before its passes, so the
// loader must refuse it: C++ counts a draw by the used length of a buffer it writes.
class Tally final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.upload<DrawCount>("drawn", 1);
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Tally>("Tally");
}
