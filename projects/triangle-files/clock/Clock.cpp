#include "contracts/Spin.h"
#include "runtime/Operator.h"

#include <cstdint>
#include <numbers>

namespace {

constexpr float turn = 2.0f * std::numbers::pi_v<float>; // radians in a whole turn

// How far the triangle has turned at a frame, counted from the frame index, so it never
// drifts; none while frames_per_turn is 0.
float angle_at(std::uint64_t frame, std::uint32_t frames_per_turn) {
  if (frames_per_turn == 0)
    return 0.0f;
  return turn * static_cast<float>(frame % frames_per_turn) /
         static_cast<float>(frames_per_turn);
}

// Hands the corners how far the triangle has turned, each frame before they cook.
class Clock final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _spin = &node.output<VP_VIEW::Spin>("spin");
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
  }
  void cook(VP::Cook &frame) override {
    _spin->angle = angle_at(frame.index(), _frames_per_turn);
  }

  VP_VIEW::Spin *_spin{};
  std::uint32_t _frames_per_turn = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Clock>("Clock");
}
