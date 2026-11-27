#include "contracts/Camera.h"
#include "contracts/Ground.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Params.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

// "For by him were all things created, that are in heaven, and that are in earth, visible
// and invisible, whether they be thrones, or dominions, or principalities, or powers: all
// things were created by him, and for him:" Colossians 1:16

// How far a drag across the picture's shorter side turns the scene: half a turn.
constexpr float drag_turn = std::numbers::pi_v<float>;
constexpr float whole_turn = 2.0f * std::numbers::pi_v<float>;
// How much nearer a step of the wheel brings the eye, and how near and far it goes, as a
// share of where the params put it.
constexpr float wheel_step = 1.1f;
constexpr float nearest_share = 0.01f;
constexpr float farthest_share = 100.0f;
// Below this, the eye, the target and up lie on one line, which leaves no frame.
constexpr float straight = 1e-6f;
// Level turning tilts the eye at most this far above or below the target's horizon, short
// of straight up or down, where the line of sight would run along up and leave no frame.
constexpr float steepest_tilt = 0.5f * std::numbers::pi_v<float> - 0.01f;
// A press of the left button again soon after, as a double click's second.
constexpr std::uint32_t double_press = 2;
constexpr std::string_view ground_port = "ground";

// The keys that glide while held, and the way each goes in the picture laid flat: x to
// its right, y the way it faces.
struct Glide {
  std::string_view key;
  glm::vec2 way;
};
constexpr std::array glides{Glide{"w", {0.0f, 1.0f}},
                            Glide{"up", {0.0f, 1.0f}},
                            Glide{"s", {0.0f, -1.0f}},
                            Glide{"down", {0.0f, -1.0f}},
                            Glide{"a", {-1.0f, 0.0f}},
                            Glide{"left", {-1.0f, 0.0f}},
                            Glide{"d", {1.0f, 0.0f}},
                            Glide{"right", {1.0f, 0.0f}}};
// A glide eases to the way the keys say, most of the way in glide_ease seconds, e^-1 of
// it left; below still_glide of glide_speed, with no key held, it stops.
constexpr float glide_ease = 0.15f;
constexpr float still_glide = 0.001f;
// The most seconds a frame's glide counts, so a frame that came late leaps no farther.
constexpr float longest_glide_step = 0.1f;
// The eye nearer than this to where the last glide left it: nothing else moved it.
constexpr float least_move = 1e-4f;

// A ground as a camera reads it, its axes laid flat on the plane square to up: where a
// point lies on its grid, as a column and a row, and the ground's height there along up,
// bilinear between its points; none off the grid. One whose heights do not fill its grid,
// or whose axes span no plane, is refused naming why (A02).
class Grid {
public:
  Grid(const VP_VIEW::Ground &ground, const glm::vec3 &up)
      : _ground(ground), _up(up), _across(flat(ground.across)),
        _along(flat(ground.along)) {
    const std::size_t points = std::size_t{ground.size.x} * ground.size.y;
    if (points == 0 || ground.heights.size() != points)
      throw std::runtime_error(std::format("ground: {} heights for a grid of {} by {}",
                                           ground.heights.size(),
                                           ground.size.x,
                                           ground.size.y));
    _determinant = glm::dot(_across, _across) * glm::dot(_along, _along) -
                   glm::dot(_across, _along) * glm::dot(_across, _along);
    if (!(_determinant > straight))
      throw std::runtime_error("ground: across and along span no plane square to up");
  }

  glm::vec2 at(const glm::vec3 &point) const {
    const glm::vec3 from = point - _ground.origin;
    const float across = glm::dot(from, _across);
    const float along = glm::dot(from, _along);
    const float aa = glm::dot(_across, _across);
    const float ab = glm::dot(_across, _along);
    const float bb = glm::dot(_along, _along);
    return {(across * bb - along * ab) / _determinant,
            (along * aa - across * ab) / _determinant};
  }
  // The way a move of cells on the grid goes, laid flat.
  glm::vec3 way(const glm::vec2 &cells) const {
    return _across * cells.x + _along * cells.y;
  }
  glm::vec2 last() const {
    return glm::vec2(_ground.size) - 1.0f;
  }
  std::optional<float> height(const glm::vec3 &point) const {
    const glm::vec2 cell = at(point);
    if (glm::any(glm::lessThan(cell, glm::vec2(0.0f))) ||
        glm::any(glm::greaterThan(cell, last())))
      return std::nullopt;
    const glm::uvec2 low(glm::floor(cell));
    const glm::uvec2 high = glm::min(low + 1u, _ground.size - 1u);
    const glm::vec2 share = cell - glm::vec2(low);
    const auto at_point = [&](glm::uvec2 grid_point) {
      return _ground.heights[std::size_t{grid_point.y} * _ground.size.x + grid_point.x];
    };
    const float first_row = glm::mix(at_point(low), at_point({high.x, low.y}), share.x);
    const float next_row = glm::mix(at_point({low.x, high.y}), at_point(high), share.x);
    const glm::vec3 on_grid =
        _ground.origin + _ground.across * cell.x + _ground.along * cell.y;
    return glm::dot(on_grid, _up) + glm::mix(first_row, next_row, share.y);
  }

private:
  glm::vec3 flat(const glm::vec3 &vector) const {
    return vector - glm::dot(vector, _up) * _up;
  }

  const VP_VIEW::Ground &_ground;
  glm::vec3 _up;
  glm::vec3 _across;
  glm::vec3 _along;
  float _determinant = 0.0f;
};

// A camera a person turns and pans about a target with the pointer: a left drag turns
// the scene, as a ball under the pointer rolls or, with turn level, about up and across
// the picture, a right or middle drag moves the target across the picture, the scene
// following the pointer, the wheel draws the eye nearer or farther, and a double left
// press brings it home, unless double_click is none, where its params put it: at eye,
// looking at target, up pointing up the picture, lens how far the picture is from the eye
// for a half height of 1. W, A, S and D or the arrows, held, glide it over the plane
// square to up at glide_speed, and over a ground a connection hands it. It reads the
// pointer and the keys only where a GUI hands its view them: in perform mode, or in a
// Perform pane while that panel's interact is on. On top, it turns about up by itself
// every frames_per_turn frames, counted from the frame index so it never drifts (C01).
// It writes one Camera a frame: the eye, the frame it looks along, the lens, and in the
// two spare members near and far, which a draw's depth counts from and to.
class Camera final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _camera = node.upload<VP_VIEW::Camera>("camera", 1);
    const glm::vec3 target = node.param<glm::vec3>("target");
    const glm::vec3 eye = node.param<glm::vec3>("eye");
    const glm::vec3 up = node.param<glm::vec3>("up");
    _lens = node.param<float>("lens");
    if (!(_lens > 0.0f)) // pan divides by it
      throw std::runtime_error(std::format("param lens = {}: beyond 0", _lens));
    const std::string turn = node.param<std::string>("turn");
    check_word("turn", turn, {"ball", "level"});
    _level = turn == "level";
    // none leaves a double click to the view, as a game's that acts on what it lands on.
    const std::string double_click = node.param<std::string>("double_click");
    check_word("double_click", double_click, {"home", "none"});
    _homes = double_click != "none";
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
    _near = node.param<float>("near");
    _far = node.param<float>("far");
    if (!(_near > 0.0f && _far > _near))
      throw std::runtime_error("near must lie beyond 0, and far beyond near");
    _glide_speed = node.param<float>("glide_speed");
    _ground_margin = node.param<float>("ground_margin");
    if (!(_glide_speed >= 0.0f && _ground_margin >= 0.0f))
      throw std::runtime_error("glide_speed and ground_margin must be 0 or more");
    _ground = grounded(node) ? &node.input<VP_VIEW::Ground>(ground_port) : nullptr;
    const glm::vec3 ahead = target - eye;
    if (glm::length(ahead) < straight ||
        glm::length(glm::cross(glm::normalize(ahead), up)) < straight)
      throw std::runtime_error(
          "eye, target and up leave no frame: eye must lie off the target, and up off "
          "the line between them");
    // Every node binds again as its view's schedule is built anew, after any edit of the
    // view and as its picture moves between the window and a pane, so a camera a person
    // turned stays turned: it goes home only as its params move home.
    if (!_placed || target != _home_target || eye != _home_eye || up != _home_up) {
      _home_target = target;
      _home_eye = eye;
      _home_up = up;
      _placed = true;
      home();
    }
  }

  void cook(VP::Cook &frame) override {
    const glm::vec2 size(frame.resolution());
    const float across = std::max(std::min(size.x, size.y), 1.0f);
    const glm::quat spin = glm::angleAxis(-spin_at(frame.index()), _up);
    for (const VP::Event &event : frame.input().events())
      follow(event, across, spin);
    glide(spin * _turned, since_before(frame.seconds()));
    const glm::quat spun = spin * _turned;
    frame.write(_camera)[0] = {
        .eye = glm::vec4(_target + spun * glm::vec3(0.0f, 0.0f, _distance), 1.0f),
        .ahead = glm::vec4(spun * glm::vec3(0.0f, 0.0f, -1.0f), _lens),
        .right = glm::vec4(spun * glm::vec3(1.0f, 0.0f, 0.0f), _near),
        .up = glm::vec4(spun * glm::vec3(0.0f, 1.0f, 0.0f), _far)};
  }

  // Whether a connection hands it a ground to keep over.
  static bool grounded(VP::Bind &node) {
    const std::string name = node.name();
    return std::ranges::any_of(node.view().connections, [&](const VP::Connection &made) {
      return std::ranges::any_of(made.to, [&](const VP::Endpoint &to) {
        return to.node == name && to.port == ground_port;
      });
    });
  }

  // The seconds since the frame before, as the run counts them, at most
  // longest_glide_step; none on the first frame.
  float since_before(double now) {
    const std::optional<double> before = std::exchange(_seconds, now);
    return before
               ? std::clamp(static_cast<float>(now - *before), 0.0f, longest_glide_step)
               : 0.0f;
  }

  // How far a whole turn every frames_per_turn frames has come at a frame; none while
  // frames_per_turn is 0.
  float spin_at(std::uint64_t frame) const {
    if (_frames_per_turn == 0)
      return 0.0f;
    return whole_turn * static_cast<float>(frame % _frames_per_turn) /
           static_cast<float>(_frames_per_turn);
  }

  // As the params put it: the frame looking from eye at target, as a rotation from the
  // camera's own, which looks down its -z with y up, and how far the eye is.
  void home() {
    const glm::vec3 ahead = glm::normalize(_home_target - _home_eye);
    const glm::vec3 right = glm::normalize(glm::cross(ahead, _home_up));
    _turned = glm::quat_cast(glm::mat3(right, glm::cross(right, ahead), -ahead));
    _target = _home_target;
    _up = glm::normalize(_home_up);
    _distance = glm::length(_home_target - _home_eye);
    _homed = _distance;
  }

  void follow(const VP::Event &event, float across, const glm::quat &spin) {
    if (event.kind == VP::Event::Kind::pointer) {
      const glm::vec2 moved = event.at - _pointer;
      _pointer = event.at;
      if (_turning && !_level)
        roll(moved, across);
      else if (_turning)
        level(moved, across);
      else if (_panning)
        pan(moved, across, spin * _turned);
    } else if (event.kind == VP::Event::Kind::button) {
      if (event.name == "left" && event.down && event.presses == double_press && _homes)
        home();
      else if (event.name == "left")
        _turning = event.down;
      else if (event.name == "right" || event.name == "middle")
        _panning = event.down;
    } else if (event.kind == VP::Event::Kind::wheel) {
      _distance = std::clamp(_distance * std::pow(wheel_step, -event.turn.y),
                             _homed * nearest_share,
                             _homed * farthest_share);
    } else if (event.kind == VP::Event::Kind::key) {
      const auto glide =
          std::ranges::find(glides, std::string_view(event.name), &Glide::key);
      if (glide != glides.end())
        _held[static_cast<std::size_t>(glide - glides.begin())] = event.down;
    } else if (event.kind == VP::Event::Kind::focus && !event.down) {
      _held.fill(false); // a key let go elsewhere never comes back up here
    }
  }

  // The keys' glide: the target, and the eye with it, the way the keys held say, eased,
  // across the plane square to up as the picture faces, turned by the spin as the
  // picture is; two keys together glide between their ways and no faster, and opposite
  // ones cancel. It never turns the camera, and the eye keeps its height along up but
  // over a ground.
  void glide(const glm::quat &facing, float seconds) {
    glm::vec2 wanted(0.0f);
    for (std::size_t key = 0; key < glides.size(); ++key)
      if (_held[key] && _glide_speed > 0.0f)
        wanted += glides[key].way;
    if (glm::length(wanted) > 1.0f)
      wanted = glm::normalize(wanted);
    _gliding += (wanted - _gliding) * (1.0f - std::exp(-seconds / glide_ease));
    if (wanted == glm::vec2(0.0f) && glm::length(_gliding) < still_glide) {
      _gliding = glm::vec2(0.0f);
      return;
    }
    const auto [right, ahead] = flat_ways(facing);
    glm::vec3 step = (right * _gliding.x + ahead * _gliding.y) * (_glide_speed * seconds);
    const glm::vec3 eye = _target + facing * glm::vec3(0.0f, 0.0f, _distance);
    if (_ground)
      step = over_ground(step, eye);
    _target += step;
    _glided_to = eye + step;
  }

  // The picture's right and the way it faces, laid flat on the plane square to up. On its
  // side, as a ball's turning may leave it, its right stands along up, and its up laid
  // flat is the way it faces.
  std::pair<glm::vec3, glm::vec3> flat_ways(const glm::quat &facing) const {
    const glm::vec3 right = flat(facing * glm::vec3(1.0f, 0.0f, 0.0f));
    if (glm::length(right) > straight)
      return {glm::normalize(right), glm::cross(_up, glm::normalize(right))};
    const glm::vec3 ahead = glm::normalize(flat(facing * glm::vec3(0.0f, 1.0f, 0.0f)));
    return {glm::cross(ahead, _up), ahead};
  }

  glm::vec3 flat(const glm::vec3 &vector) const {
    return vector - glm::dot(vector, _up) * _up;
  }

  // Over the ground: the target stops at its edge and slides along it, or, past the edge
  // as a pan may leave it, glides back but no farther out. The eye rides ground_margin
  // over the ground where it rises to that under it, and comes back down to its own
  // height where it falls away, a height anything else that moves the eye sets anew.
  glm::vec3 over_ground(glm::vec3 step, const glm::vec3 &eye) {
    const Grid grid(*_ground, _up);
    const glm::vec2 from = grid.at(_target);
    const glm::vec2 to = glm::clamp(grid.at(_target + step),
                                    glm::min(from, glm::vec2(0.0f)),
                                    glm::max(from, grid.last()));
    step = grid.way(to - from);
    if (glm::distance(eye, _glided_to) > least_move)
      _lifted = 0.0f;
    const float now = glm::dot(eye, _up);
    const float own = now - _lifted;
    const std::optional<float> ground = grid.height(eye + step);
    const float over = ground ? std::max(own, *ground + _ground_margin) : own;
    _lifted = over - own;
    return step + (over - now) * _up;
  }

  // The scene turns about the axis across the drag, in the camera's frame, so the side
  // the pointer pressed moves with it, a drag across the picture's shorter side half a
  // turn; the eye goes the other way round the target.
  void roll(glm::vec2 moved, float across) {
    const float length = glm::length(moved);
    if (length == 0.0f)
      return;
    const glm::vec3 axis = glm::normalize(glm::vec3(moved.y, moved.x, 0.0f));
    _turned =
        glm::normalize(_turned * glm::angleAxis(-drag_turn * length / across, axis));
  }

  // A drag across turns the scene about up through the target, a drag down tilts the eye
  // up over it, both as roll turns them, but about up and the picture's horizontal only,
  // which stays level, so the horizon never tips; the tilt stops short of straight down.
  void level(glm::vec2 moved, float across) {
    const glm::vec3 ahead = _turned * glm::vec3(0.0f, 0.0f, -1.0f);
    const float tilt = std::asin(std::clamp(glm::dot(ahead, _up), -1.0f, 1.0f));
    const float tilted =
        std::clamp(tilt - drag_turn * moved.y / across, -steepest_tilt, steepest_tilt);
    const glm::vec3 right = _turned * glm::vec3(1.0f, 0.0f, 0.0f);
    _turned = glm::normalize(glm::angleAxis(-drag_turn * moved.x / across, _up) *
                             glm::angleAxis(tilted - tilt, right) * _turned);
  }

  // The target moves across the picture, so the scene at its depth follows the pointer:
  // at the target, the shorter side spans 2 * distance / lens. Along the frame shown,
  // spun by frames_per_turn too: roll and level turn the scene in the camera's own frame
  // or about up, which the spin leaves alike, but the target moves in the world's.
  void pan(glm::vec2 moved, float across, const glm::quat &shown) {
    const float pixel = 2.0f * _distance / (_lens * across);
    const glm::vec3 right = shown * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = shown * glm::vec3(0.0f, 1.0f, 0.0f);
    _target -= (right * moved.x - up * moved.y) * pixel;
  }

  VP::Upload<VP_VIEW::Camera> _camera;
  bool _placed = false; // home set by a bind
  glm::vec3 _home_target{};
  glm::vec3 _home_eye{};
  glm::vec3 _home_up{};
  float _lens = 0.0f;
  bool _level = false; // turn level, else ball
  bool _homes = true;  // a double click brings it home
  std::uint32_t _frames_per_turn = 0;
  float _near = 0.0f;
  float _far = 0.0f;
  glm::quat _turned{};    // from the camera's own frame to the world's
  glm::vec3 _target{};    // what it turns about
  glm::vec3 _up{};        // what turning by itself and level turning turn about, unit
  float _distance = 0.0f; // of the eye from the target
  float _homed = 0.0f;    // as the params put it, which the wheel's reach counts from
  glm::vec2 _pointer{};   // as the events so far left it
  bool _turning = false;  // a left drag
  bool _panning = false;  // a right or middle drag
  float _glide_speed = 0.0f;
  float _ground_margin = 0.0f;
  const VP_VIEW::Ground *_ground = nullptr; // while a connection hands it one
  std::array<bool, glides.size()> _held{};  // each key that glides, down
  glm::vec2 _gliding{}; // eased, a share of glide_speed: x to the right, y ahead
  std::optional<double> _seconds; // the frame before's
  glm::vec3 _glided_to{};         // the eye, where the last glide left it
  float _lifted = 0.0f;           // how far a ground raised the eye over its own height
};

} // namespace

VP_OPERATOR(Camera);
