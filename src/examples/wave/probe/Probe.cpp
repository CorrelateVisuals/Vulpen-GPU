#include "runtime/Operator.h"

#include <array>
#include <cstdint>
#include <format>

namespace {

constexpr std::size_t line_capacity = 256;

// Prints what the GPU wrote every so many frames: a headless view is seen through it.
// Each value prints as the shortest text that reads back to the same bits, so two runs
// that print the same lines computed the same output (C01).
class Probe final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _samples = node.readback<float>("samples");
    _every = node.param<std::uint32_t>("every");
  }
  void cook(VP::Cook &frame) override {
    if (_every == 0 || frame.index() % _every != 0)
      return;
    std::array<char, line_capacity> line{};
    char *end =
        std::format_to_n(line.data(), line.size(), "frame {:6}:", frame.index()).out;
    for (const float sample : frame.read(_samples))
      end = std::format_to_n(end, line.data() + line.size() - end, " {:+}", sample).out;
    frame.log(VP::Level::info, {line.data(), end});
  }

  VP::Readback<float> _samples;
  std::uint32_t _every = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Probe>("Probe");
}
