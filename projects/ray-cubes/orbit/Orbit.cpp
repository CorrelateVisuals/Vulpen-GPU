#include "contracts/Camera.h"
#include "runtime/Operator.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

namespace {

constexpr float turn = 2.0f * std::numbers::pi_v<float>; // radians in a whole turn
constexpr glm::vec3 upward{0.0f, 1.0f, 0.0f};

// How far a whole turn every frames_per_turn frames has come at a frame, counted from the
// frame index, so it never drifts; none while frames_per_turn is 0.
float angle_at(std::uint64_t frame, std::uint32_t frames_per_turn) {
  if (frames_per_turn == 0)
    return 0.0f;
  return turn * static_cast<float>(frame % frames_per_turn) /
         static_cast<float>(frames_per_turn);
}

// The camera, on the CPU, as one element a frame is all there is to it: on a circle about
// the target, at a height, looking at it. No shader of this node reads it, so it asks
// room for one, which the connection takes to rays.
class Orbit final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _camera = node.upload<VP_VIEW::Camera>("camera", 1);
    _target = node.param<glm::vec3>("target");
    _distance = node.param<float>("distance");
    _height = node.param<float>("height");
    _lens = node.param<float>("lens");
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
  }

  void cook(VP::Cook &frame) override {
    const float angle = angle_at(frame.index(), _frames_per_turn);
    const glm::vec3 eye =
        _target + glm::vec3(_distance * std::cos(angle), _height, _distance * std::sin(angle));
    const glm::vec3 ahead = glm::normalize(_target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(ahead, upward));
    frame.write(_camera)[0] = {.eye = glm::vec4(eye, 1.0f),
                               .ahead = glm::vec4(ahead, _lens),
                               .right = glm::vec4(right, 0.0f),
                               .up = glm::vec4(glm::cross(right, ahead), 0.0f)};
  }

  VP::Upload<VP_VIEW::Camera> _camera;
  glm::vec3 _target{};
  float _distance = 0.0f;
  float _height = 0.0f;
  float _lens = 0.0f;
  std::uint32_t _frames_per_turn = 0;
};

} // namespace

VP_OPERATOR(Orbit);
