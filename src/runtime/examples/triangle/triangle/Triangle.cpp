#include "runtime/Operator.h"

#include <cmath>
#include <numbers>

namespace {

constexpr float radius = 0.8f; // of the circle the corners ride on, in clip space
constexpr float turn = 2.0f * std::numbers::pi_v<float>;
constexpr float opaque = 1.0f;

// Between 0 and 1, following the angle round.
float swing(float angle) {
  return (1.0f + std::cos(angle)) / 2.0f;
}

// The triangle lives on the CPU: C++ writes its corners and its tint, and the shaders
// only draw them.
class Triangle final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _corners = node.upload<glm::vec2>("corners");
    _tint = node.value<glm::vec4>("tint");
    _speed = node.param<float>("speed");
  }
  void cook(VP::Cook &frame) override {
    _angle = std::fmod(_angle + _speed, turn);
    const std::span<glm::vec2> corners = frame.write(_corners);
    const float step = turn / static_cast<float>(corners.size());
    for (std::size_t index = 0; index < corners.size(); ++index) {
      const float at = _angle + step * static_cast<float>(index);
      corners[index] = radius * glm::vec2(std::cos(at), std::sin(at));
    }
    frame.set(
        _tint,
        glm::vec4(swing(_angle), swing(_angle + step), swing(_angle - step), opaque));
  }

  VP::Upload<glm::vec2> _corners;
  VP::Value<glm::vec4> _tint;
  float _speed = 0;
  float _angle = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Triangle>("Triangle");
}
