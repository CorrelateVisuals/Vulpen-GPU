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

// The clock and nothing more: C++ works out how far the triangle has turned and sets it
// in the pass block, and the shader does everything else.
class Raster final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _angle = node.value<float>("angle");
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
  }
  void cook(VP::Cook &frame) override {
    frame.set(_angle, angle_at(frame.index(), _frames_per_turn));
  }

  VP::Value<float> _angle;
  std::uint32_t _frames_per_turn = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Raster>("Raster");
}
