#include "contracts/Camera.h"
#include "runtime/Operator.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>

namespace {

// How far a drag across the picture's shorter side turns the scene: half a turn.
constexpr float drag_turn = std::numbers::pi_v<float>;
// How much nearer a step of the wheel brings the eye, and how near and far it goes, as a
// share of where the params put it.
constexpr float wheel_step = 1.1f;
constexpr float nearest_share = 0.01f;
constexpr float farthest_share = 100.0f;
// Below this, the eye, the target and up lie on one line, which leaves no frame.
constexpr float straight = 1e-6f;
// A press of the left button again soon after, as a double click's second.
constexpr std::uint32_t double_press = 2;

// A camera a person turns and pans about a target with the pointer, as an arcball does: a
// left drag turns the scene as if a ball under the pointer were pressed and rolled, a
// right or middle drag moves the target across the picture, the scene following the
// pointer, the wheel draws the eye nearer or farther, and a double left press brings it
// home, where its params put it: at eye, looking at target, up pointing up the picture,
// lens how far the picture is from the eye for a half height of 1. It reads the presses
// only where a GUI hands its view the pointer: in perform mode, or in a Perform pane
// while that panel's interact is on. It writes what ray-cubes' rays read, one element a
// frame: the eye, the frame it looks along, and the lens.
class Arcball final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _camera = node.upload<VP_VIEW::Camera>("camera", 1);
    _home_target = node.param<glm::vec3>("target");
    _home_eye = node.param<glm::vec3>("eye");
    _home_up = node.param<glm::vec3>("up");
    _lens = node.param<float>("lens");
    const glm::vec3 ahead = _home_target - _home_eye;
    if (glm::length(ahead) < straight ||
        glm::length(glm::cross(glm::normalize(ahead), _home_up)) < straight)
      throw std::runtime_error(
          "eye, target and up leave no frame: eye must lie off the target, and up off "
          "the line between them");
    home();
  }

  void cook(VP::Cook &frame) override {
    const glm::vec2 size(frame.resolution());
    const float across = std::max(std::min(size.x, size.y), 1.0f);
    for (const VP::Event &event : frame.input().events())
      follow(event, across);
    const glm::vec3 eye = _target + _turned * glm::vec3(0.0f, 0.0f, _distance);
    frame.write(_camera)[0] = {
        .eye = glm::vec4(eye, 1.0f),
        .ahead = glm::vec4(_turned * glm::vec3(0.0f, 0.0f, -1.0f), _lens),
        .right = glm::vec4(_turned * glm::vec3(1.0f, 0.0f, 0.0f), 0.0f),
        .up = glm::vec4(_turned * glm::vec3(0.0f, 1.0f, 0.0f), 0.0f)};
  }

  // As the params put it: the frame looking from eye at target, as a rotation from the
  // camera's own, which looks down its -z with y up, and how far the eye is.
  void home() {
    const glm::vec3 ahead = glm::normalize(_home_target - _home_eye);
    const glm::vec3 right = glm::normalize(glm::cross(ahead, _home_up));
    _turned = glm::quat_cast(glm::mat3(right, glm::cross(right, ahead), -ahead));
    _target = _home_target;
    _distance = glm::length(_home_target - _home_eye);
    _homed = _distance;
  }

  void follow(const VP::Event &event, float across) {
    if (event.kind == VP::Event::Kind::pointer) {
      const glm::vec2 moved = event.at - _pointer;
      _pointer = event.at;
      if (_turning)
        turn(moved, across);
      else if (_panning)
        pan(moved, across);
    } else if (event.kind == VP::Event::Kind::button) {
      if (event.name == "left" && event.down && event.presses == double_press)
        home();
      else if (event.name == "left")
        _turning = event.down;
      else if (event.name == "right" || event.name == "middle")
        _panning = event.down;
    } else if (event.kind == VP::Event::Kind::wheel) {
      _distance = std::clamp(_distance * std::pow(wheel_step, -event.turn.y),
                             _homed * nearest_share,
                             _homed * farthest_share);
    }
  }

  // The scene turns about the axis across the drag, in the camera's frame, so the side
  // the pointer pressed moves with it, a drag across the picture's shorter side half a
  // turn; the eye goes the other way round the target.
  void turn(glm::vec2 moved, float across) {
    const float length = glm::length(moved);
    if (length == 0.0f)
      return;
    const glm::vec3 axis = glm::normalize(glm::vec3(moved.y, moved.x, 0.0f));
    _turned =
        glm::normalize(_turned * glm::angleAxis(-drag_turn * length / across, axis));
  }

  // The target moves across the picture, so the scene at its depth follows the pointer:
  // at the target, the shorter side spans 2 * distance / lens.
  void pan(glm::vec2 moved, float across) {
    const float pixel = 2.0f * _distance / (_lens * across);
    const glm::vec3 right = _turned * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = _turned * glm::vec3(0.0f, 1.0f, 0.0f);
    _target -= (right * moved.x - up * moved.y) * pixel;
  }

  VP::Upload<VP_VIEW::Camera> _camera;
  glm::vec3 _home_target{};
  glm::vec3 _home_eye{};
  glm::vec3 _home_up{};
  float _lens = 0.0f;
  glm::quat _turned{};    // from the camera's own frame to the world's
  glm::vec3 _target{};    // what it turns about
  float _distance = 0.0f; // of the eye from the target
  float _homed = 0.0f;    // as the params put it, which the wheel's reach counts from
  glm::vec2 _pointer{};   // as the events so far left it
  bool _turning = false;  // a left drag
  bool _panning = false;  // a right or middle drag
};

} // namespace

VP_OPERATOR(Arcball);
