#include "runtime/Operator.h"

#include <cmath>
#include <numbers>

namespace {

constexpr double turn = 2 * std::numbers::pi;

// The phase follows from the frame index, so it lasts across frames and shader swaps and
// stays exact for years: a float that adds a step each frame drifts within hours and
// stops within days (A03). The shader turns it into values.
class Wave final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _phase = node.value<float>("phase");
    _speed = node.param<float>("speed");
  }
  void cook(VP::Cook &frame) override {
    const double phase = std::fmod(_speed * static_cast<double>(frame.index()), turn);
    frame.set(_phase, static_cast<float>(phase));
  }

  VP::Value<float> _phase;
  float _speed = 0;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Wave>("Wave");
}
