#include "runtime/Operator.h"

#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

constexpr double unpaced_rate = 60; // frames a second, as an unpaced run counts time

// Reads back what its pass saw of the frame block, a frame after the GPU wrote it, and
// stops when it differs from what the engine wrote: an offset off, or a value missing.
class Frame final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _seen = node.readback<float>("seen");
  }
  void cook(VP::Cook &frame) override {
    const std::span<const float> seen = frame.read(_seen);
    if (seen.empty())
      return;
    const std::uint64_t index = frame.index() - 1;
    const auto time = static_cast<float>(static_cast<double>(index) / unpaced_rate);
    if (seen[0] != 0 || seen[1] != 0 || seen[2] != time ||
        seen[3] != static_cast<float>(index))
      throw std::runtime_error(std::format(
          "the frame block held {} {} {} {}, where the engine wrote 0 0 {} {}",
          seen[0],
          seen[1],
          seen[2],
          seen[3],
          time,
          index));
  }

  VP::Readback<float> _seen;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Frame>("Frame");
}
