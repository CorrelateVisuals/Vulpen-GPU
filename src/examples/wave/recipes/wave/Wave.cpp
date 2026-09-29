#include "runtime/Operator.h"

namespace {

// The phase lives on the CPU, where state lasts across frames and shader swaps; the
// shader turns it into values.
class Wave final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _phase = node.value<float>("phase");
    _speed = node.param<float>("speed");
  }
  void cook(VP::Cook &frame) override {
    _now += _speed;
    frame.set(_phase, _now);
  }

  VP::Value<float> _phase;
  float _speed = 0;
  float _now = 0;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Wave>("Wave");
}
